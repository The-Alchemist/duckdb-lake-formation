#include "lake_formation_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "catalog/rest/iceberg_access_delegation.hpp"
#include "lakeformation/lake_formation_provider.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &instance = loader.GetDatabaseInstance();

	// The provider is implemented against the iceberg extension's headers and registers into its plugin
	// registry, so iceberg must be present. httpfs/aws provide S3 object access and the base AWS
	// credential chain used to call Glue/Lake Formation.
	ExtensionHelper::AutoLoadExtension(instance, "iceberg");
	if (!instance.ExtensionIsLoaded("iceberg")) {
		throw MissingExtensionException("The lake_formation extension requires the iceberg extension to be loaded!");
	}
	ExtensionHelper::AutoLoadExtension(instance, "httpfs");
	ExtensionHelper::AutoLoadExtension(instance, "aws");

	// Register the Lake Formation access-delegation provider. The registry lives on the database-wide
	// ObjectCache, shared with the iceberg extension, so iceberg can dispatch to us during ATTACH/scan.
	auto &registry = IcebergPluginRegistry::GetOrCreate(instance);
	registry.RegisterProvider(make_shared_ptr<LakeFormationProvider>());
}

void LakeFormationExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

string LakeFormationExtension::Name() {
	return "lake_formation";
}

string LakeFormationExtension::Version() const {
#ifdef EXT_VERSION_LAKE_FORMATION
	return EXT_VERSION_LAKE_FORMATION;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(lake_formation, loader) {
	duckdb::LoadInternal(loader);
}
}
