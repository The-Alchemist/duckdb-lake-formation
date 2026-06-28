#pragma once

#include "catalog/rest/iceberg_access_delegation.hpp"
#include "catalog/rest/catalog_entry/table/iceberg_table_information.hpp"

#include "lakeformation/lf_client.hpp"
#include "lakeformation/lf_types.hpp"

namespace duckdb {

//! Per-table Lake Formation state cached on IcebergTableInformation::delegation_state. Holds the
//! caller-specific data-filter policy (discovered via Glue) and the resolved Glue/LF identifiers.
struct LakeFormationTableState : public IcebergDelegationTableState {
	LakeFormationTablePolicy policy;
#ifndef EMSCRIPTEN
	LakeFormationTableIdentifiers identifiers;
#endif
	bool loaded = false;
};

//! Access-delegation provider that enforces AWS Lake Formation data filters on Glue Iceberg catalogs.
//! Registered with the iceberg extension's IcebergPluginRegistry under the name "lake_formation".
class LakeFormationProvider : public IcebergAccessDelegationProvider {
public:
	static constexpr const char *PROVIDER_NAME = "lake_formation";

	string GetProviderName() const override;
	void ValidateAttachOptions(IcebergAttachOptions &options) override;
	void OnTableLoaded(IcebergAccessDelegationContext &dctx) override;
	IRCAPITableCredentials GetScanCredentials(IcebergAccessDelegationContext &dctx,
	                                          optional_ptr<const vector<Value>> partition_values) override;
	void ApplyMandatoryScanFilters(IcebergAccessDelegationContext &dctx, IcebergScanInfo &scan_info,
	                               IcebergMultiFileList &file_list, const IcebergTableSchema &schema) override;
	void OnPartitionFile(IcebergAccessDelegationContext &dctx, const vector<IcebergPartitionInfo> &partition_info,
	                     const string &file_path) override;

private:
	LakeFormationTableState &GetState(IcebergAccessDelegationContext &dctx);
	void EnsurePolicyLoaded(IcebergAccessDelegationContext &dctx);
	IRCAPITableCredentials BuildCredentials(IcebergAccessDelegationContext &dctx,
	                                        optional_ptr<const vector<Value>> partition_values);
};

} // namespace duckdb
