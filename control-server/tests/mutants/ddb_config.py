"""control_plane.md 7.6 DynamoDB 호출 설정의 변이."""

P = "controlplane/__main__.py"
K = ["tests/test_ddb_config.py::test_build_store_passes_call_limits"]

MUTANTS = [
    {"id": "ddb-config-dropped", "path": P, "old": "return Store.from_env(environ, config=config)",
     "new": "return Store.from_env(environ)", "kills": K,
     "why": "7.6 설정을 넘기지 않으면 boto3 기본값(60초, 재시도)으로 돈다"},
    {"id": "ddb-sdk-retries", "path": P, "old": '"total_max_attempts": DDB_MAX_ATTEMPTS}',
     "new": '"total_max_attempts": 3}', "kills": K,
     "why": "7.6 재시도는 클라이언트가 한다. SDK 가 다시 보내면 요청 한 건이 8.2 시간 제한을 넘는다"},
    {"id": "ddb-read-timeout", "path": P, "old": "read_timeout=DDB_READ_TIMEOUT_S", "new": "read_timeout=60",
     "kills": K, "why": "7.6 read 시간 제한"},
]
