﻿/*
 * Copyright (c) 2016 The ZLToolKit project authors. All Rights Reserved.
 *
 * This file is part of ZLToolKit(https://github.com/xia-chu/ZLToolKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "Buffer.h"
#include "Util/onceToken.h"

namespace toolkit {

StatisticImp(Buffer);
StatisticImp(BufferRaw);
StatisticImp(BufferLikeString);
StatisticImp(BufferList);

BufferRaw::Ptr BufferRaw::create(){
#if 0
    static ResourcePool<BufferRaw> packet_pool;
    static onceToken token([]() {
        packet_pool.setSize(1024);
    });
    auto ret = packet_pool.obtain();
    ret->setSize(0);
    return ret;
#else
    return Ptr(new BufferRaw);
#endif
}

///////////////BufferList/////////////////////

bool BufferList::empty() {
    return _iovec_off == _iovec.size();
}

size_t BufferList::count(){
    return _iovec.size() - _iovec_off;
}

#if defined(_WIN32)
int sendmsg(int fd, const struct msghdr *msg, int flags) {
    int n = 0;
    int total = 0;
    for(auto i = 0; i != msg->msg_iovlen ; ++i ){
        do {
            n = sendto(fd,(char *)msg->msg_iov[i].iov_base,msg->msg_iov[i].iov_len,flags,(struct sockaddr *)msg->msg_name,msg->msg_namelen);
        }while (-1 == n && UV_EINTR == get_uv_error(true));

        if(n == -1 ){
            //可能一个字节都未发送成功
            return total ? total : -1;
        }
        if(n < msg->msg_iov[i].iov_len){
            //发送部分字节成功
            return total + n;
        }
        //单次全部发送成功
        total += n;
    }
    //全部发送成功
    return total;
}
#endif // defined(_WIN32)

ssize_t BufferList::send_l(int fd, int flags,bool udp) {
    ssize_t n;
    do {
        struct msghdr msg;
        if (!udp) {
            msg.msg_name = NULL;
            msg.msg_namelen = 0;
        } else {
            auto &buffer = _pkt_list.front();
            msg.msg_name = buffer->_addr;
            msg.msg_namelen = buffer->_addr_len;
        }

        msg.msg_iov = &(_iovec[_iovec_off]);
        msg.msg_iovlen = (decltype(msg.msg_iovlen)) (_iovec.size() - _iovec_off);
        size_t max = udp ? 1 : IOV_MAX;
        if (msg.msg_iovlen > max) {
            msg.msg_iovlen = max;
        }
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
        msg.msg_flags = flags;
        n = sendmsg(fd, &msg, flags);
    } while (-1 == n && UV_EINTR == get_uv_error(true));

    if (n >= (ssize_t) _remainSize) {
        //全部写完了
        _iovec_off = _iovec.size();
        _remainSize = 0;
        _pkt_list.for_each([](BufferSock::Ptr &buffer){
            buffer->onSendSuccess();
        });
        return n;
    }

    if (n > 0) {
        //部分发送成功
        reOffset(n);
        return n;
    }

    //一个字节都未发送
    return n;
}

ssize_t BufferList::send(int fd, int flags, bool udp) {
    auto remainSize = _remainSize;
    while (_remainSize && send_l(fd, flags, udp) != -1);

    ssize_t sent = remainSize - _remainSize;
    if (sent > 0) {
        //部分或全部发送成功
        return sent;
    }
    //一个字节都未发送成功
    return -1;
}

void BufferList::reOffset(size_t n) {
    _remainSize -= n;
    size_t offset = 0;
    auto last_off = _iovec_off;
    for (auto i = _iovec_off; i != _iovec.size(); ++i) {
        auto &ref = _iovec[i];
        offset += ref.iov_len;
        if (offset < n) {
            continue;
        }
        ssize_t remain = offset - n;
        ref.iov_base = (char *) ref.iov_base + ref.iov_len - remain;
        ref.iov_len = (decltype(ref.iov_len)) remain;
        _iovec_off = i;
        if (remain == 0) {
            _iovec_off += 1;
        }
        break;
    }

    //删除已经发送的数据，节省内存
    for (auto i = last_off; i < _iovec_off; ++i) {
        auto &front = _pkt_list.front();
        front->onSendSuccess();
        _pkt_list.pop_front();
    }
}

BufferList::BufferList(List<BufferSock::Ptr> &list) : _iovec(list.size()) {
    _pkt_list.swap(list);
    auto it = _iovec.begin();
    _pkt_list.for_each([&](BufferSock::Ptr &buffer) {
        it->iov_base = buffer->data();
        it->iov_len = (decltype(it->iov_len)) buffer->size();
        _remainSize += it->iov_len;
        ++it;
    });
}

BufferSock::BufferSock(Buffer::Ptr buffer, struct sockaddr *addr, int addr_len, onResult cb) {
    if (addr && addr_len) {
        _addr = (struct sockaddr *) malloc(addr_len);
        memcpy(_addr, addr, addr_len);
        _addr_len = addr_len;
    }
    assert(buffer);
    _buffer = std::move(buffer);
    _result = std::move(cb);
}

BufferSock::~BufferSock(){
    if (_addr) {
        free(_addr);
    }
    if (_result) {
        _result(0);
        _result = nullptr;
    }
}

char *BufferSock::data() const {
    return _buffer->data();
}

size_t BufferSock::size() const {
    return _buffer->size();
}

void BufferSock::onSendSuccess() {
    if (_result) {
        _result(size());
        _result = nullptr;
    }
}

void BufferSock::setSendResult(onResult cb){
    _result = std::move(cb);
}

}//namespace toolkit