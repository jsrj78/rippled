//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_SERVER_BASEHTTPPEER_H_INCLUDED
#define RIPPLE_SERVER_BASEHTTPPEER_H_INCLUDED

#include <ripple/basics/Log.h>
#include <ripple/server/Session.h>
#include <ripple/server/impl/Door.h>
#include <ripple/server/impl/io_list.h>
#include <ripple/server/impl/ServerImpl.h>
#include <beast/asio/IPAddressConversion.h>
#include <beast/asio/placeholders.h>
#include <beast/asio/error.h> // for is_short_read?
#include <beast/http/message.h>
#include <beast/http/parser.h>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/spawn.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace ripple {

/** Represents an active connection. */
template <class Impl>
class BaseHTTPPeer
    : public io_list::work
    , public Session
{
protected:
    using clock_type = std::chrono::system_clock;
    using error_code = boost::system::error_code;
    using endpoint_type = boost::asio::ip::tcp::endpoint;
    using waitable_timer = boost::asio::basic_waitable_timer <clock_type>;
    using yield_context = boost::asio::yield_context;

    enum
    {
        // Size of our read/write buffer
        bufferSize = 4 * 1024,

        // Max seconds without completing a message
        timeoutSeconds = 30

    };

    struct buffer
    {
        buffer (void const* ptr, std::size_t len)
            : data (new char[len])
            , bytes (len)
            , used (0)
        {
            memcpy (data.get(), ptr, len);
        }

        std::unique_ptr <char[]> data;
        std::size_t bytes;
        std::size_t used;
    };

    Port const& port_;
    Handler& handler_;
    boost::asio::io_service::work work_;
    boost::asio::io_service::strand strand_;
    waitable_timer timer_;
    endpoint_type remote_address_;
    beast::Journal journal_;

    std::string id_;
    std::size_t nid_;

    boost::asio::streambuf read_buf_;
    beast::http::message message_;
    beast::http::body body_;
    std::vector<buffer> wq_;
    std::vector<buffer> wq2_;
    std::mutex mutex_;
    bool graceful_ = false;
    bool complete_ = false;
    boost::system::error_code ec_;

    clock_type::time_point when_;
    int request_count_ = 0;
    std::size_t bytes_in_ = 0;
    std::size_t bytes_out_ = 0;

    //--------------------------------------------------------------------------

public:
    template <class ConstBufferSequence>
    BaseHTTPPeer (Port const& port, Handler& handler,
        boost::asio::io_service& io_service, beast::Journal journal,
            endpoint_type remote_address, ConstBufferSequence const& buffers);

    virtual
    ~BaseHTTPPeer();

    Session&
    session()
    {
        return *this;
    }

    void close() override;

protected:
    Impl&
    impl()
    {
        return *static_cast<Impl*>(this);
    }

    void
    fail (error_code ec, char const* what);

    void
    start_timer();

    void
    cancel_timer();

    void
    on_timer (error_code ec);

    void
    do_read (yield_context yield);

    void
    on_write(error_code const& ec,
        std::size_t bytes_transferred);

    void
    do_writer (std::shared_ptr <Writer> const& writer,
        bool keep_alive, yield_context yield);

    virtual
    void
    do_request() = 0;

    virtual
    void
    do_close() = 0;

    // Session

    beast::Journal
    journal() override
    {
        return journal_;
    }

    Port const&
    port() override
    {
        return port_;
    }

    beast::IP::Endpoint
    remoteAddress() override
    {
        return beast::IPAddressConversion::from_asio(remote_address_);
    }

    beast::http::message&
    request() override
    {
        return message_;
    }

    beast::http::body const&
    body() override
    {
        return body_;
    }

    void
    write (void const* buffer, std::size_t bytes) override;

    void
    write (std::shared_ptr <Writer> const& writer,
        bool keep_alive) override;

    std::shared_ptr<Session>
    detach() override;

    void
    complete() override;

    void
    close (bool graceful) override;
};

//------------------------------------------------------------------------------

template <class Impl>
template <class ConstBufferSequence>
BaseHTTPPeer<Impl>::BaseHTTPPeer (Port const& port, Handler& handler,
    boost::asio::io_service& io_service, beast::Journal journal,
        endpoint_type remote_address,
        ConstBufferSequence const& buffers)
    : port_(port)
    , handler_(handler)
    , work_ (io_service)
    , strand_ (io_service)
    , timer_ (io_service)
    , remote_address_ (remote_address)
    , journal_ (journal)
{
    read_buf_.commit(boost::asio::buffer_copy(read_buf_.prepare (
        boost::asio::buffer_size (buffers)), buffers));
    static std::atomic <int> sid;
    nid_ = ++sid;
    id_ = std::string("#") + std::to_string(nid_) + " ";
    JLOG(journal_.trace) << id_ <<
        "accept:    " << remote_address_.address();
    when_ = clock_type::now();
}

template <class Impl>
BaseHTTPPeer<Impl>::~BaseHTTPPeer()
{
    handler_.onClose(session(), ec_);
    JLOG(journal_.trace) << id_ <<
        "destroyed: " << request_count_ <<
            ((request_count_ == 1) ? " request" : " requests");
}

template <class Impl>
void
BaseHTTPPeer<Impl>::close()
{
    if (! strand_.running_in_this_thread())
        return strand_.post(std::bind(
            (void(BaseHTTPPeer::*)(void))&BaseHTTPPeer::close,
                impl().shared_from_this()));
    error_code ec;
    impl().stream_.lowest_layer().close(ec);
}

//------------------------------------------------------------------------------

template <class Impl>
void
BaseHTTPPeer<Impl>::fail (error_code ec, char const* what)
{
    if (! ec_ && ec != boost::asio::error::operation_aborted)
    {
        ec_ = ec;
        JLOG(journal_.trace) << id_ <<
            std::string(what) << ": " << ec.message();
        impl().stream_.lowest_layer().close (ec);
    }
}

template <class Impl>
void
BaseHTTPPeer<Impl>::start_timer()
{
    error_code ec;
    timer_.expires_from_now (std::chrono::seconds(timeoutSeconds), ec);
    if (ec)
        return fail (ec, "start_timer");
    timer_.async_wait (strand_.wrap (std::bind (
        &BaseHTTPPeer<Impl>::on_timer, impl().shared_from_this(),
            beast::asio::placeholders::error)));
}

// Convenience for discarding the error code
template <class Impl>
void
BaseHTTPPeer<Impl>::cancel_timer()
{
    error_code ec;
    timer_.cancel(ec);
}

// Called when session times out
template <class Impl>
void
BaseHTTPPeer<Impl>::on_timer (error_code ec)
{
    if (ec == boost::asio::error::operation_aborted)
        return;
    if (! ec)
        ec = boost::system::errc::make_error_code (
            boost::system::errc::timed_out);
    fail (ec, "timer");
}

//------------------------------------------------------------------------------

template <class Impl>
void
BaseHTTPPeer<Impl>::do_read (yield_context yield)
{
    complete_ = false;

    error_code ec;
    bool eof = false;
    body_.clear();
    beast::http::parser parser (message_, body_, true);
    for(;;)
    {
        if (read_buf_.size() == 0)
        {
            start_timer();
            auto const bytes_transferred = boost::asio::async_read (
                impl().stream_, read_buf_.prepare (bufferSize),
                    boost::asio::transfer_at_least(1), yield[ec]);
            cancel_timer();

            eof = ec == boost::asio::error::eof;
            if (eof)
            {
                ec = error_code{};
            }
            else if (! ec)
            {
                bytes_in_ += bytes_transferred;
                read_buf_.commit (bytes_transferred);
            }
        }

        if (! ec)
        {
            if (! eof)
            {
                // VFALCO TODO Currently parsing errors are treated the
                //             same as the connection dropping. Instead, we
                // should request that the handler compose a proper HTTP error
                // response. This requires refactoring HTTPReply() into
                // something sensible.
                std::size_t used;
                std::tie (ec, used) = parser.write (read_buf_.data());
                if (! ec)
                    read_buf_.consume (used);
            }
            else
            {
                ec = parser.write_eof();
            }
        }

        if (! ec)
        {
            if (parser.complete())
                return do_request();
            if (eof)
                ec = boost::asio::error::eof; // incomplete request
        }

        if (ec)
            return fail (ec, "read");
    }
}

// Send everything in the write queue.
// The write queue must not be empty upon entry.
template<class Impl>
void
BaseHTTPPeer<Impl>::on_write(error_code const& ec,
    std::size_t bytes_transferred)
{
    cancel_timer();
    if(ec)
        return fail(ec, "write");
    bytes_out_ += bytes_transferred;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wq2_.clear();
        wq2_.reserve(wq_.size());
        std::swap(wq2_, wq_);
    }
    if(! wq2_.empty())
    {
        std::vector<boost::asio::const_buffer> v;
        v.reserve(wq2_.size());
        for(auto const& b : wq2_)
            v.emplace_back(b.data.get(), b.bytes);
        start_timer();
        using namespace beast::asio;
        return boost::asio::async_write(impl().stream_, v,
            strand_.wrap(std::bind(&BaseHTTPPeer::on_write,
                impl().shared_from_this(), placeholders::error,
                    placeholders::bytes_transferred)));
    }
    if (! complete_)
        return;
    if (graceful_)
        return do_close();
    boost::asio::spawn(strand_,
        std::bind (&BaseHTTPPeer<Impl>::do_read,
            impl().shared_from_this(), std::placeholders::_1));
}

template <class Impl>
void
BaseHTTPPeer<Impl>::do_writer (std::shared_ptr <Writer> const& writer,
    bool keep_alive, yield_context yield)
{
    std::function <void(void)> resume;
    {
        auto const p = impl().shared_from_this();
        resume = std::function <void(void)>(
            [this, p, writer, keep_alive]()
            {
                boost::asio::spawn (strand_, std::bind (
                    &BaseHTTPPeer<Impl>::do_writer, p, writer, keep_alive,
                        std::placeholders::_1));
            });
    }

    for(;;)
    {
        if (! writer->prepare (bufferSize, resume))
            return;
        error_code ec;
        auto const bytes_transferred = boost::asio::async_write (
            impl().stream_, writer->data(), boost::asio::transfer_at_least(1),
                yield[ec]);
        if (ec)
            return fail (ec, "writer");
        writer->consume(bytes_transferred);
        if (writer->complete())
            break;
    }

    if (! keep_alive)
        return do_close();

    boost::asio::spawn (strand_, std::bind (&BaseHTTPPeer<Impl>::do_read,
        impl().shared_from_this(), std::placeholders::_1));
}

//------------------------------------------------------------------------------

// Send a copy of the data.
template <class Impl>
void
BaseHTTPPeer<Impl>::write(
    void const* buffer, std::size_t bytes)
{
    if (bytes == 0)
        return;
    if([&]
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wq_.emplace_back(buffer, bytes);
            return wq_.size() == 1 && wq2_.size() == 0;
        }())
    {
        if (strand_.running_in_this_thread())
            return strand_.post(std::bind(
                &BaseHTTPPeer::on_write,
                    impl().shared_from_this(),
                        error_code{}, 0));
        else
            return on_write(error_code{}, 0);
    }
}

template <class Impl>
void
BaseHTTPPeer<Impl>::write (std::shared_ptr <Writer> const& writer,
    bool keep_alive)
{
    boost::asio::spawn (strand_, std::bind (
        &BaseHTTPPeer<Impl>::do_writer, impl().shared_from_this(),
            writer, keep_alive, std::placeholders::_1));
}

// DEPRECATED
// Make the Session asynchronous
template <class Impl>
std::shared_ptr<Session>
BaseHTTPPeer<Impl>::detach()
{
    return impl().shared_from_this();
}

// DEPRECATED
// Called to indicate the response has been written (but not sent)
template <class Impl>
void
BaseHTTPPeer<Impl>::complete()
{
    if (! strand_.running_in_this_thread())
        return strand_.post(std::bind (&BaseHTTPPeer<Impl>::complete,
            impl().shared_from_this()));

    message_ = beast::http::message{};
    complete_ = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (! wq_.empty() && ! wq2_.empty())
            return;
    }

    // keep-alive
    boost::asio::spawn (strand_, std::bind (&BaseHTTPPeer<Impl>::do_read,
        impl().shared_from_this(), std::placeholders::_1));
}

// DEPRECATED
// Called from the Handler to close the session.
template <class Impl>
void
BaseHTTPPeer<Impl>::close (bool graceful)
{
    if (! strand_.running_in_this_thread())
        return strand_.post(std::bind(
            (void(BaseHTTPPeer::*)(bool))&BaseHTTPPeer<Impl>::close,
                impl().shared_from_this(), graceful));

    complete_ = true;
    if (graceful)
    {
        graceful_ = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (! wq_.empty() || ! wq2_.empty())
                return;
        }
        return do_close();
    }

    error_code ec;
    impl().stream_.lowest_layer().close (ec);
}

} // ripple

#endif