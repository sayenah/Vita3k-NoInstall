string(TIMESTAMP BUILD_DATE "%Y-%m-%d")
LIST(APPEND FREEDESKTOP_RELEASE_INFO
    "<release version=\"${GIT_COUNT}-${VITA3K_GIT_REV}\" date=\"${BUILD_DATE}\" />"
)

configure_file(
    ${CMAKE_SOURCE_DIR}/dist/linux/${VITA3K_APP_ID}.metainfo.xml.in
    ${CMAKE_SOURCE_DIR}/dist/linux/${VITA3K_APP_ID}.metainfo.xml
)
