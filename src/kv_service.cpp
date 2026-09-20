// SPDX-License-Identifier: Apache-2.0

#include "kv_service.h"

#include <brpc/closure_guard.h>
#include <butil/logging.h>

namespace minikv {
namespace {

void SetStatus(const rocksdb::Status& status, Response* response) {
    if (status.ok()) {
        response->set_code(OK);
    } else if (status.IsNotFound()) {
        response->set_code(NOT_FOUND);
        response->set_message("key not found");
    } else if (status.IsInvalidArgument()) {
        response->set_code(INVALID_ARGUMENT);
        response->set_message(status.ToString());
    } else {
        LOG(ERROR) << "RocksDB: " << status.ToString();
        response->set_code(STORAGE_ERROR);
        response->set_message("storage operation failed");
    }
}

}  // namespace

void KVServiceImpl::Put(google::protobuf::RpcController*, const PutRequest* request,
                        Response* response, google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);
    SetStatus(store_.Put(request->key(), request->value(), request->ttl_ms()), response);
}

void KVServiceImpl::Get(google::protobuf::RpcController*, const KeyRequest* request,
                        Response* response, google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);
    std::string value;
    const auto status = store_.Get(request->key(), &value);
    SetStatus(status, response);
    if (status.ok()) response->set_value(value);
}

void KVServiceImpl::Delete(google::protobuf::RpcController*, const KeyRequest* request,
                           Response* response, google::protobuf::Closure* done) {
    brpc::ClosureGuard guard(done);
    SetStatus(store_.Delete(request->key()), response);
}

}  // namespace minikv
