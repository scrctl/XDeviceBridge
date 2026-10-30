#include "decode/Decoder.h"

namespace xdb {

/// 未提供平台硬解后端的构建使用软件工厂。
/// 未编入 libavcodec 时返回 nullptr，调用方需检查结果。
#if !defined(__ANDROID__) || !defined(XDB_HAVE_LIBAV)
std::unique_ptr<Decoder> create_platform_decoder() {
    return create_software_decoder();
}

#endif

}  // namespace xdb
