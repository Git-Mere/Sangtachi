# Wintun 배포물의 기대값 (windows-prereq.md 4절 Wintun DLL 과 서명).
#
# 판정 기준과 절차는 그 절이 갖고, 값은 여기에만 둔다. 버전을 올리면 네 값을 함께 바꾼다.
# 받아서 비교하는 빌드 단계는 아직 없다. roadmap.md Phase 6 작업이다.

set(SANGTACHI_WINTUN_VERSION "0.14.1")
set(SANGTACHI_WINTUN_URL "https://www.wintun.net/builds/wintun-${SANGTACHI_WINTUN_VERSION}.zip")

# 공식 사이트가 공개하는 값이다. zip 의 해시이고 DLL 의 해시가 아니다.
set(SANGTACHI_WINTUN_ZIP_SHA256 "07c256185d6ee3652e09fa55c0b673e2624b565e02c4b9091c79ca7d2f24ef51")

# 위 해시가 맞는 zip 에서 꺼낸 bin/amd64/wintun.dll 의 해시다. 공개된 값이 아니다.
set(SANGTACHI_WINTUN_DLL_SHA256 "e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce")

# 그 DLL 의 서명 인증서 Subject 전체 문자열이다.
set(SANGTACHI_WINTUN_SIGNER_SUBJECT "CN=WireGuard LLC, O=WireGuard LLC, L=Boulder, S=Colorado, C=US, SERIALNUMBER=4227913, OID.2.5.4.15=Private Organization, OID.1.3.6.1.4.1.311.60.2.1.2=Ohio, OID.1.3.6.1.4.1.311.60.2.1.3=US")
