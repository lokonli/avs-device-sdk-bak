add_definitions("-DACSDK_LOG_MODULE=nSKeyWordDetector")
add_library(NS SHARED
    NSKeyWordDetector.cpp)

target_include_directories(NS PUBLIC
	"${NS_KEY_WORD_DETECTOR_INCLUDE_DIR}"
	"${KWD_SOURCE_DIR}/include"
    	"${DefaultClient_SOURCE_DIR}/include"
	"${NS_SOURCE_DIR}/include")

target_link_libraries(NS KWD AVSCommon DefaultClient "${NS_KEY_WORD_DETECTOR_LIB_PATH}" blas)

# install target
asdk_install()