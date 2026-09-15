#include "grcd.hpp"

Result mtGrcdOpen(Service* out) {
    if (serviceIsActive(out)) return 0;
    Result rc = smGetService(out, "grc:d");
    if (R_FAILED(rc)) serviceClose(out);
    return rc;
}
void mtGrcdClose(Service* svc) { serviceClose(svc); }
Result mtGrcdBegin(Service* svc) { return serviceDispatch(svc, 1); }
Result mtGrcdTransfer(Service* svc, GrcStream stream, void* buffer, size_t size,
                      u32* num_frames, u32* data_size, u64* start_timestamp) {
    struct { u32 num_frames; u32 data_size; u64 start_timestamp; } out{};
    u32 in = static_cast<u32>(stream);
    Result rc = serviceDispatchInOut(svc, 2, in, out,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, size } });
    if (R_SUCCEEDED(rc)) {
        if (num_frames) *num_frames = out.num_frames;
        if (data_size) *data_size = out.data_size;
        if (start_timestamp) *start_timestamp = out.start_timestamp;
    }
    return rc;
}
