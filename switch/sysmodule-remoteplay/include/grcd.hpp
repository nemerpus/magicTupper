#pragma once
#include <switch.h>

Result mtGrcdOpen(Service* out);
void mtGrcdClose(Service* svc);
Result mtGrcdBegin(Service* svc);
Result mtGrcdTransfer(Service* svc, GrcStream stream, void* buffer, size_t size,
                      u32* num_frames, u32* data_size, u64* start_timestamp);
