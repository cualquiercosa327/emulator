#pragma once

#include "socket.hpp"

#include <string_view>

namespace network
{
    class tcp_server_socket;

    class tcp_client_socket : public socket
    {
        // TODO: Construct and connect client!

        tcp_client_socket() = default;
        ~tcp_client_socket() = default;

        tcp_client_socket(tcp_client_socket&& obj) noexcept = default;
        tcp_client_socket& operator=(tcp_client_socket&& obj) noexcept = default;

        [[maybe_unused]] bool send(const void* data, size_t size) const;
        [[maybe_unused]] bool send(std::string_view data) const;
        bool receive(std::string& data) const;

        address get_target() const;

      private:
        address target_{};

        friend tcp_server_socket;
        tcp_client_socket(SOCKET s, const address& target);
    };
}
