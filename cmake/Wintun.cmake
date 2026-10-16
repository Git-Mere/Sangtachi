# Wintun 배포물의 기대값 (windows-prereq.md 4절 Wintun DLL 과 서명).
#
# 판정 기준과 절차는 그 절이 갖고, 값은 여기에만 둔다. 버전을 올리면 네 값을 함께 바꾼다.
# 아래 sangtachi_fetch_wintun() 이 배포물을 받아 세 판정을 돌린다.

set(SANGTACHI_WINTUN_VERSION "0.14.1")
set(SANGTACHI_WINTUN_URL "https://www.wintun.net/builds/wintun-${SANGTACHI_WINTUN_VERSION}.zip")

# 공식 사이트가 공개하는 값이다. zip 의 해시이고 DLL 의 해시가 아니다.
set(SANGTACHI_WINTUN_ZIP_SHA256 "07c256185d6ee3652e09fa55c0b673e2624b565e02c4b9091c79ca7d2f24ef51")

# 위 해시가 맞는 zip 에서 꺼낸 bin/amd64/wintun.dll 의 해시다. 공개된 값이 아니다.
set(SANGTACHI_WINTUN_DLL_SHA256 "e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce")

# 그 DLL 의 서명 인증서 Subject 전체 문자열이다.
set(SANGTACHI_WINTUN_SIGNER_SUBJECT "CN=WireGuard LLC, O=WireGuard LLC, L=Boulder, S=Colorado, C=US, SERIALNUMBER=4227913, OID.2.5.4.15=Private Organization, OID.1.3.6.1.4.1.311.60.2.1.2=Ohio, OID.1.3.6.1.4.1.311.60.2.1.3=US")

# 배포물을 받아 windows-prereq.md 4절의 세 판정을 돌린다. 하나라도 어긋나면 설정이 멈춘다.
#
# 결과로 둘을 정한다.
#   SANGTACHI_WINTUN_DLL      판정을 지난 bin/amd64/wintun.dll 의 경로
#   SANGTACHI_WINTUN_INCLUDE  같은 zip 의 include 폴더 (wintun.h)
#
# zip 은 한 번만 받는다. 판정은 설정할 때마다 돈다. 받아 둔 파일이 바뀌었어도 걸러야 한다.
function(sangtachi_fetch_wintun)
    set(root "${CMAKE_BINARY_DIR}/_deps/wintun-${SANGTACHI_WINTUN_VERSION}")
    set(zip "${root}/wintun-${SANGTACHI_WINTUN_VERSION}.zip")
    set(dll "${root}/wintun/bin/amd64/wintun.dll")

    if(NOT EXISTS "${zip}")
        file(DOWNLOAD "${SANGTACHI_WINTUN_URL}" "${zip}.part"
             EXPECTED_HASH SHA256=${SANGTACHI_WINTUN_ZIP_SHA256}
             TLS_VERIFY ON
             STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${zip}.part")
            message(FATAL_ERROR "Wintun download failed: ${status}")
        endif()
        file(RENAME "${zip}.part" "${zip}")
    endif()

    # 판정 1. zip 해시가 공개값과 같다. 받아 둔 zip 도 다시 본다.
    file(SHA256 "${zip}" zip_hash)
    if(NOT zip_hash STREQUAL SANGTACHI_WINTUN_ZIP_SHA256)
        message(FATAL_ERROR "Wintun zip hash ${zip_hash} != expected ${SANGTACHI_WINTUN_ZIP_SHA256}")
    endif()

    file(REMOVE_RECURSE "${root}/wintun")
    file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${root}")

    # 판정 3. DLL 해시가 기대값과 같다.
    file(SHA256 "${dll}" dll_hash)
    if(NOT dll_hash STREQUAL SANGTACHI_WINTUN_DLL_SHA256)
        message(FATAL_ERROR "wintun.dll hash ${dll_hash} != expected ${SANGTACHI_WINTUN_DLL_SHA256}")
    endif()

    # 판정 1, 2. 서명 Status 와 Subject 전체 문자열. Windows PowerShell 의 Get-AuthenticodeSignature 다.
    # PSModulePath 를 지운다. pwsh 에서 띄운 빌드는 그 값이 pwsh 의 모듈 경로를 가리키고, 그러면
    # Windows PowerShell 이 보안 모듈을 불러오지 못한다. 실측으로 겪었다.
    # 경로는 PowerShell 작은따옴표 문자열에 들어간다. 경로의 작은따옴표는 둘로 적는다.
    string(REPLACE "'" "''" dll_literal "${dll}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command
                "$s = Get-AuthenticodeSignature -LiteralPath '${dll_literal}'; [Console]::Out.Write($s.Status.ToString() + '|' + $s.SignerCertificate.Subject)"
        OUTPUT_VARIABLE signature
        ERROR_VARIABLE signature_error
        RESULT_VARIABLE signature_code)
    if(NOT signature_code EQUAL 0)
        message(FATAL_ERROR "wintun.dll signature check did not run (${signature_code}): ${signature_error}")
    endif()
    if(NOT signature STREQUAL "Valid|${SANGTACHI_WINTUN_SIGNER_SUBJECT}")
        message(FATAL_ERROR "wintun.dll signature '${signature}' != 'Valid|${SANGTACHI_WINTUN_SIGNER_SUBJECT}'")
    endif()

    set(SANGTACHI_WINTUN_DLL "${dll}" PARENT_SCOPE)
    set(SANGTACHI_WINTUN_INCLUDE "${root}/wintun/include" PARENT_SCOPE)
endfunction()
