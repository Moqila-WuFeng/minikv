// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "minikv.pb.h"
#include "store.h"

namespace minikv {

class KVServiceImpl final : public KVService {
public:
    explicit KVServiceImpl(Store& store) : store_(store) {}
    void Put(google::protobuf::RpcController*, const PutRequest*, Response*,
             google::protobuf::Closure*) override;
    void Get(google::protobuf::RpcController*, const KeyRequest*, Response*,
             google::protobuf::Closure*) override;
    void Delete(google::protobuf::RpcController*, const KeyRequest*, Response*,
                google::protobuf::Closure*) override;

private:
    Store& store_;
};

}  // namespace minikv
