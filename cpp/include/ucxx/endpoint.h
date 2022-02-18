/**
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See file LICENSE for terms.
 */
#pragma once

#include <netdb.h>

#include <memory>
#include <string>

#include <ucp/api/ucp.h>

#include <ucxx/address.h>
#include <ucxx/component.h>
#include <ucxx/exception.h>
#include <ucxx/listener.h>
#include <ucxx/sockaddr_utils.h>
#include <ucxx/transfer_tag.h>
#include <ucxx/utils.h>
#include <ucxx/worker.h>

namespace ucxx
{

class UCXXRequest;

struct EpParamsDeleter {
    void operator()(ucp_ep_params_t* ptr)
    {
        if (ptr != nullptr && ptr->field_mask & UCP_EP_PARAM_FIELD_FLAGS)
            sockaddr_utils_free(&ptr->sockaddr);
    }
};

void _err_cb(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    ucs_status_t* _status = (ucs_status_t*)arg;
    *_status = status;
    std::cout << "Error callback called with status " << status << "(" << ucs_status_string(status) << ")" << std::endl;
}

class UCXXEndpoint : public UCXXComponent
{
    private:
        ucp_ep_h _handle{nullptr};
        ucs_status_t _status{UCS_OK};
        bool _endpoint_error_handling{true};

    UCXXEndpoint(
            std::shared_ptr<UCXXComponent> worker_or_listener,
            std::unique_ptr<ucp_ep_params_t, EpParamsDeleter> params,
            bool endpoint_error_handling
        ) : _endpoint_error_handling{endpoint_error_handling}
    {
        auto worker = UCXXEndpoint::getWorker(worker_or_listener);

        if (worker == nullptr || worker->get_handle() == nullptr)
            throw ucxx::UCXXError("Worker not initialized");

        setParent(worker_or_listener);

        params->err_mode = UCP_ERR_HANDLING_MODE_NONE;
        params->err_handler.cb = _err_cb;
        params->err_handler.arg = &_status;

        assert_ucs_status(ucp_ep_create(worker->get_handle(), params.get(), &_handle));
    }

    public:

    UCXXEndpoint() = default;

    UCXXEndpoint(const UCXXEndpoint&) = delete;
    UCXXEndpoint& operator=(UCXXEndpoint const&) = delete;

    UCXXEndpoint(UCXXEndpoint&& o) noexcept
        : _handle{std::exchange(o._handle, nullptr)}
    {
    }

    UCXXEndpoint& operator=(UCXXEndpoint&& o) noexcept
    {
        this->_handle = std::exchange(o._handle, nullptr);

        return *this;
    }

    ~UCXXEndpoint()
    {
        if (_handle == nullptr)
            return;

        // Close the endpoint
        unsigned close_mode = UCP_EP_CLOSE_MODE_FLUSH;
        if (_endpoint_error_handling and _status != UCS_OK)
        {
            // We force close endpoint if endpoint error handling is enabled and
            // the endpoint status is not UCS_OK
            close_mode = UCP_EP_CLOSE_MODE_FORCE;
        }
        ucs_status_ptr_t status = ucp_ep_close_nb(_handle, close_mode);
        if (UCS_PTR_IS_PTR(status))
        {
            auto worker = UCXXEndpoint::getWorker(_parent);
            while (ucp_request_check_status(status) == UCS_INPROGRESS)
                worker->progress();
            ucp_request_free(status);
        }
        else if (UCS_PTR_STATUS(status) != UCS_OK)
        {
            std::cerr << "Error while closing endpoint: " << ucs_status_string(UCS_PTR_STATUS(status)) << std::endl;
        }
    }

    static std::shared_ptr<UCXXWorker> getWorker(std::shared_ptr<UCXXComponent> worker_or_listener)
    {
        auto worker = std::dynamic_pointer_cast<UCXXWorker>(worker_or_listener);
        if (worker == nullptr)
        {
            auto listener = std::dynamic_pointer_cast<UCXXListener>(worker_or_listener);
            worker = std::dynamic_pointer_cast<UCXXWorker>(listener->getParent());
        }
        return worker;
    }

    friend std::shared_ptr<UCXXEndpoint> createEndpointFromHostname(
            std::shared_ptr<UCXXWorker> worker,
            std::string ip_address,
            uint16_t port,
            bool endpoint_error_handling
        )
    {
        if (worker == nullptr || worker->get_handle() == nullptr)
            throw ucxx::UCXXError("Worker not initialized");

        auto params = std::unique_ptr<ucp_ep_params_t, EpParamsDeleter>(new ucp_ep_params_t);

        struct hostent *hostname = gethostbyname(ip_address.c_str());
        if (hostname== nullptr)
            throw ucxx::UCXXError(std::string("Invalid IP address or hostname"));

        params->field_mask = UCP_EP_PARAM_FIELD_FLAGS |
            UCP_EP_PARAM_FIELD_SOCK_ADDR |
            UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
            UCP_EP_PARAM_FIELD_ERR_HANDLER;
        params->flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
        if (sockaddr_utils_set(&params->sockaddr, hostname->h_name, port))
            throw std::bad_alloc();

        return std::shared_ptr<UCXXEndpoint>(new UCXXEndpoint(worker, std::move(params), endpoint_error_handling));
    }

    friend std::shared_ptr<UCXXEndpoint> createEndpointFromConnRequest(
            std::shared_ptr<UCXXListener> listener,
            ucp_conn_request_h conn_request,
            bool endpoint_error_handling
        )
    {
        if (listener == nullptr || listener->get_handle() == nullptr)
            throw ucxx::UCXXError("Worker not initialized");

        auto params = std::unique_ptr<ucp_ep_params_t, EpParamsDeleter>(new ucp_ep_params_t);
        params->field_mask = UCP_EP_PARAM_FIELD_FLAGS |
            UCP_EP_PARAM_FIELD_CONN_REQUEST |
            UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
            UCP_EP_PARAM_FIELD_ERR_HANDLER;
        params->flags = UCP_EP_PARAMS_FLAGS_NO_LOOPBACK;
        params->conn_request = conn_request;

        return std::shared_ptr<UCXXEndpoint>(new UCXXEndpoint(listener, std::move(params), endpoint_error_handling));
    }


    friend std::shared_ptr<UCXXEndpoint> createEndpointFromWorkerAddress(
            std::shared_ptr<UCXXWorker> worker,
            std::shared_ptr<UCXXAddress> address,
            bool endpoint_error_handling
        )
    {
        if (worker == nullptr || worker->get_handle() == nullptr)
            throw ucxx::UCXXError("Worker not initialized");
        if (address == nullptr || address->getHandle() == nullptr || address->getLength() == 0)
            throw ucxx::UCXXError("Address not initialized");

        auto params = std::unique_ptr<ucp_ep_params_t, EpParamsDeleter>(new ucp_ep_params_t);
        params->field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
            UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
            UCP_EP_PARAM_FIELD_ERR_HANDLER;
        params->address = address->getHandle();

        return std::shared_ptr<UCXXEndpoint>(new UCXXEndpoint(worker, std::move(params), endpoint_error_handling));
    }

    ucp_ep_h getHandle()
    {
        return _handle;
    }

    std::shared_ptr<UCXXRequest> createRequest(std::shared_ptr<ucxx_request_t> request)
    {
        auto endpoint = std::dynamic_pointer_cast<UCXXEndpoint>(shared_from_this());
        return ucxx::createRequest(endpoint, request);
    }

    std::shared_ptr<UCXXRequest> tag_send(void* buffer, size_t length, ucp_tag_t tag)
    {
        auto worker = UCXXEndpoint::getWorker(_parent);
        auto request = tag_msg(worker->get_handle(), _handle, true, buffer, length, tag);
        return createRequest(request);
    }

    std::shared_ptr<UCXXRequest> tag_recv(void* buffer, size_t length, ucp_tag_t tag)
    {
        auto worker = UCXXEndpoint::getWorker(_parent);
        auto request = tag_msg(worker->get_handle(), _handle, false, buffer, length, tag);
        return createRequest(request);
    }

};

}  // namespace ucxx