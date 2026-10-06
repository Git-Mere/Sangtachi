"""오류 코드와 HTTP 상태. 출처는 control_plane.md 4.1 공통 봉투의 오류 표다."""

from __future__ import annotations

# 오류 코드 -> HTTP 상태. too_large 는 본문(413)과 헤더(431) 둘이라 여기에 두지 않고
# 부르는 쪽이 status 를 준다.
STATUS: dict[str, int] = {
    "bad_request": 400,
    "method_not_allowed": 405,
    "unknown_op": 404,
    "length_required": 411,
    "room_not_found": 404,
    "room_expired": 410,
    "room_full": 409,
    "unauthorized": 403,
    "rate_limited": 429,
    "internal": 500,
    "unavailable": 503,
}

TOO_LARGE_STATUS = frozenset({413, 431})


class OpError(Exception):
    """연산이나 파싱이 오류 봉투로 끝나는 경우. message 에 요청 내용을 넣지 않는다 (4.1)."""

    def __init__(self, code: str, message: str = "", status: int | None = None) -> None:
        if code == "too_large":
            if status not in TOO_LARGE_STATUS:
                raise ValueError("too_large 는 status 413 이나 431 을 받아야 한다")
        elif code in STATUS:
            if status is not None and status != STATUS[code]:
                raise ValueError(f"{code} 의 status 는 {STATUS[code]} 다")
            status = STATUS[code]
        else:
            raise ValueError(f"모르는 오류 코드 {code!r}")
        super().__init__(code)
        self.code = code
        self.status = status
        self.message = message or code

    def envelope(self) -> dict:
        return {"ok": False, "error": self.code, "message": self.message}
