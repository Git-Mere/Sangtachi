"""4.1 공통 봉투의 오류 표와 errors.STATUS 의 대조."""

import pytest

from controlplane.errors import STATUS, OpError

# control_plane.md 4.1 의 표를 그대로 옮긴 것이 아니라 그 표와 코드가 어긋나는지 보는 대조다.
DOC_TABLE = {
    "bad_request": 400, "method_not_allowed": 405, "unknown_op": 404, "length_required": 411,
    "room_not_found": 404, "room_expired": 410, "room_full": 409, "unauthorized": 403,
    "rate_limited": 429, "internal": 500, "unavailable": 503,
}


def test_status_table_matches_doc():
    assert STATUS == DOC_TABLE


@pytest.mark.parametrize("status", [413, 431])
def test_too_large_needs_explicit_status(status):
    assert OpError("too_large", status=status).status == status


@pytest.mark.parametrize("args", [("too_large", None), ("too_large", 400), ("nope", None), ("unauthorized", 404)])
def test_rejects_bad_construction(args):
    with pytest.raises(ValueError):
        OpError(args[0], status=args[1])


def test_envelope_shape():
    assert OpError("room_full").envelope() == {"ok": False, "error": "room_full", "message": "room_full"}
