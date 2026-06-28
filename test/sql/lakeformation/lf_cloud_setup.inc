statement ok
CREATE SECRET glue_lf_secret (
    TYPE S3,
    REGION '{ICEBERG_LF_REGION}',
    PROVIDER credential_chain,
    CHAIN 'sts',
    ASSUME_ROLE_ARN '{ICEBERG_LF_TEST_ROLE_ARN}'
);

statement ok
attach '{ICEBERG_LF_CATALOG_ID}' as lf_datalake (
    TYPE ICEBERG,
    ENDPOINT_TYPE 'GLUE',
    ACCESS_DELEGATION_MODE 'lake_formation',
    LF_SESSION_TAG '{ICEBERG_LF_SESSION_TAG}'
);
