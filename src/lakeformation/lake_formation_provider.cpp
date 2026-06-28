#include "lakeformation/lake_formation_provider.hpp"

#include "lakeformation/lf_filter_parser.hpp"
#include "lakeformation/lf_table_metadata.hpp"

#include "catalog/rest/iceberg_catalog.hpp"
#include "catalog/rest/catalog_entry/schema/iceberg_schema_entry.hpp"
#include "catalog/rest/storage/authorization/sigv4.hpp"
#include "planning/iceberg_multi_file_list.hpp"
#include "planning/snapshot/iceberg_scan_info.hpp"
#include "core/metadata/partition/iceberg_partition_spec.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

string LakeFormationProvider::GetProviderName() const {
	return PROVIDER_NAME;
}

void LakeFormationProvider::ValidateAttachOptions(IcebergAttachOptions &options) {
	// LF requires a session tag that is forwarded as the LakeFormationAuthorizedCaller STS tag. Move it
	// out of the generic options map (so iceberg does not reject it) and into delegation_options, where
	// we read it back at runtime.
	string session_tag;
	for (auto it = options.options.begin(); it != options.options.end();) {
		if (StringUtil::Lower(it->first) == "lf_session_tag") {
			session_tag = it->second.ToString();
			options.delegation_options.emplace("lf_session_tag", it->second);
			it = options.options.erase(it);
		} else {
			++it;
		}
	}
	if (session_tag.empty()) {
		throw InvalidConfigurationException(
		    "LF_SESSION_TAG is required when ACCESS_DELEGATION_MODE is 'lake_formation'");
	}
}

LakeFormationTableState &LakeFormationProvider::GetState(IcebergAccessDelegationContext &dctx) {
	auto &table_info = dctx.table_info;
	if (!table_info.delegation_state) {
		table_info.delegation_state = make_shared_ptr<LakeFormationTableState>();
	}
	// Safe: this provider is the only writer of delegation_state for an LF-attached catalog, and the
	// concrete type was created in this binary.
	return static_cast<LakeFormationTableState &>(*table_info.delegation_state);
}

#ifndef EMSCRIPTEN

void LakeFormationProvider::EnsurePolicyLoaded(IcebergAccessDelegationContext &dctx) {
	auto &state = GetState(dctx);
	if (state.loaded) {
		return;
	}
	auto &table_info = dctx.table_info;
	// Glue (via GetUnfilteredTableMetadata) returns the caller's effective row/column policy. The IRC
	// table load only carries Iceberg metadata, so this is loaded once per table here.
	LakeFormationClient lf_client(dctx.context, dctx.catalog);
	state.identifiers = lf_client.GetTableIdentifiers(table_info.schema, table_info.name);
	state.policy = lf_client.FetchTablePolicy(table_info.schema, table_info.name, table_info.table_metadata);
	state.loaded = true;
}

static vector<Value> PartitionValuesFromInfo(const IcebergTableMetadata &metadata,
                                             const vector<IcebergPartitionInfo> &partition_info) {
	vector<Value> result;
	if (partition_info.empty()) {
		return result;
	}
	auto spec_id = metadata.default_spec_id;
	auto spec_it = metadata.partition_specs.find(spec_id);
	if (spec_it == metadata.partition_specs.end()) {
		throw InvalidInputException("Partition spec %d not found in table metadata", spec_id);
	}
	auto &spec = spec_it->second;
	for (auto &field : spec.fields) {
		bool found = false;
		for (auto &part : partition_info) {
			if (part.field_id == field.partition_field_id) {
				result.push_back(part.value);
				found = true;
				break;
			}
		}
		if (!found) {
			result.emplace_back(LogicalType::VARCHAR);
		}
	}
	return result;
}

static IRCAPITableCredentials BuildSecret(ClientContext &context, IcebergCatalog &catalog,
                                          const LakeFormationTemporaryCredentials &lf_credentials,
                                          const string &secret_name, const string &storage_scope) {
	IRCAPITableCredentials result;

	case_insensitive_map_t<Value> config_options;
	if (catalog.auth_handler->type == IcebergAuthorizationType::SIGV4) {
		auto &sigv4_auth = catalog.auth_handler->Cast<SIGV4Authorization>();
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
		auto catalog_credentials = context.db->GetSecretManager().GetSecretByName(transaction, sigv4_auth.secret);
		if (catalog_credentials) {
			auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*catalog_credentials->secret);
			if (!kv_secret.TryGetValue("region").IsNull()) {
				config_options.emplace("region", kv_secret.TryGetValue("region"));
			}
		}
		if (!sigv4_auth.sigv4_region.empty()) {
			config_options["region"] = Value(sigv4_auth.sigv4_region);
		}
	}

	config_options["key_id"] = Value(lf_credentials.access_key_id);
	config_options["secret"] = Value(lf_credentials.secret_access_key);
	config_options["session_token"] = Value(lf_credentials.session_token);

	if (StringUtil::StartsWith(catalog.uri, "glue")) {
		auto region = config_options["region"].ToString();
		config_options["endpoint"] = Value("s3." + region + ".amazonaws.com");
	}

	result.config = make_uniq<CreateSecretInput>();
	auto &info = *result.config;
	info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	info.persist_type = SecretPersistType::TEMPORARY;
	info.name = Identifier(secret_name);
	info.type = Identifier("s3");
	info.provider = Identifier("config");
	info.storage_type = Identifier("memory");
	info.options = config_options;
	if (!storage_scope.empty()) {
		info.scope = {storage_scope};
	}
	return result;
}

IRCAPITableCredentials LakeFormationProvider::BuildCredentials(IcebergAccessDelegationContext &dctx,
                                                               optional_ptr<const vector<Value>> partition_values) {
	EnsurePolicyLoaded(dctx);
	auto &state = GetState(dctx);
	auto &context = dctx.context;
	auto &catalog = dctx.catalog;
	auto &table_info = dctx.table_info;

	const auto &table_location = table_info.table_metadata.location;
	string storage_scope = table_location;
	if (!storage_scope.empty() && !StringUtil::EndsWith(storage_scope, "/")) {
		storage_scope += "/";
	}

	auto transaction_id = MetaTransaction::Get(context).global_transaction_id;
	auto secret_base_name = StringUtil::Format("__internal_ic_lf_%s__%s__%s__%s", table_info.table_id,
	                                           table_info.schema.name, table_info.name, to_string(transaction_id));

	LakeFormationClient lf_client(context, catalog);
	LakeFormationTemporaryCredentials lf_credentials;
	if (partition_values && !partition_values->empty()) {
		// Partitioned tables: LF scopes credentials per partition value list, not just per table ARN.
		string partition_key;
		for (auto &value : *partition_values) {
			partition_key += value.ToString() + "_";
		}
		auto secret_name = StringUtil::Format("%s_partition_%s", secret_base_name, partition_key);
		lf_credentials =
		    lf_client.GetPartitionCredentials(state.identifiers, state.policy, *partition_values, table_location);
		return BuildSecret(context, catalog, lf_credentials, secret_name, storage_scope);
	}

	lf_credentials = lf_client.GetTableCredentials(state.identifiers, state.policy, table_location);
	return BuildSecret(context, catalog, lf_credentials, secret_base_name, storage_scope);
}

void LakeFormationProvider::OnTableLoaded(IcebergAccessDelegationContext &dctx) {
	EnsurePolicyLoaded(dctx);
}

IRCAPITableCredentials LakeFormationProvider::GetScanCredentials(IcebergAccessDelegationContext &dctx,
                                                                 optional_ptr<const vector<Value>> partition_values) {
	return BuildCredentials(dctx, partition_values);
}

void LakeFormationProvider::ApplyMandatoryScanFilters(IcebergAccessDelegationContext &dctx, IcebergScanInfo &scan_info,
                                                      IcebergMultiFileList &file_list, const IcebergTableSchema &schema) {
	EnsurePolicyLoaded(dctx);
	auto &state = GetState(dctx);

	// Now that iceberg has bound the table schema, reject grant shapes we do not enforce yet
	// (column-level / cell-level permissions).
	ValidateLakeFormationPolicyV1(state.policy, schema);

	if (state.policy.row_filter_sql.empty()) {
		return;
	}
	// Compile the Glue row-filter SQL into mandatory scan filters: push column filters into the scan and
	// store a bound filter on the scan info, which the iceberg optimizer re-applies above the scan so
	// user predicates cannot widen the LF grant.
	auto filter_result = ParseLakeFormationRowFilter(dctx.context, state.policy.row_filter_sql, schema);
	scan_info.mandatory_delegation_filter_parsed = std::move(filter_result.parsed_filter);
	scan_info.mandatory_delegation_filter_bound = std::move(filter_result.bound_filter);
	for (auto &entry : filter_result.table_filters) {
		file_list.PushTableFilter(entry.first, entry.second->Copy());
	}
}

void LakeFormationProvider::OnPartitionFile(IcebergAccessDelegationContext &dctx,
                                            const vector<IcebergPartitionInfo> &partition_info,
                                            const string &file_path) {
	if (partition_info.empty()) {
		return;
	}
	// Table-level LF credentials may not cover every partition's S3 prefix; refresh the secret lazily as
	// files are discovered during manifest walks.
	auto partition_values = PartitionValuesFromInfo(dctx.table_info.table_metadata, partition_info);
	auto credentials = BuildCredentials(dctx, &partition_values);
	if (credentials.config) {
		auto &secret_manager = SecretManager::Get(dctx.context);
		(void)secret_manager.CreateSecret(dctx.context, *credentials.config);
	}
}

#else

void LakeFormationProvider::EnsurePolicyLoaded(IcebergAccessDelegationContext &dctx) {
	throw NotImplementedException("Lake Formation integration is not available in duckdb-wasm builds");
}

void LakeFormationProvider::OnTableLoaded(IcebergAccessDelegationContext &dctx) {
	throw NotImplementedException("Lake Formation integration is not available in duckdb-wasm builds");
}

IRCAPITableCredentials LakeFormationProvider::GetScanCredentials(IcebergAccessDelegationContext &dctx,
                                                                 optional_ptr<const vector<Value>> partition_values) {
	throw NotImplementedException("Lake Formation integration is not available in duckdb-wasm builds");
}

void LakeFormationProvider::ApplyMandatoryScanFilters(IcebergAccessDelegationContext &dctx, IcebergScanInfo &scan_info,
                                                      IcebergMultiFileList &file_list, const IcebergTableSchema &schema) {
}

void LakeFormationProvider::OnPartitionFile(IcebergAccessDelegationContext &dctx,
                                            const vector<IcebergPartitionInfo> &partition_info,
                                            const string &file_path) {
}

IRCAPITableCredentials LakeFormationProvider::BuildCredentials(IcebergAccessDelegationContext &dctx,
                                                               optional_ptr<const vector<Value>> partition_values) {
	throw NotImplementedException("Lake Formation integration is not available in duckdb-wasm builds");
}

#endif

} // namespace duckdb
