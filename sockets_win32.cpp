
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include "sockets.hpp"

// This is the Windows Implementation of the sockets.hpp sockets.

namespace jup {

Socket_context::Socket_context() {
	WSADATA wsaData;
	assert(WSAStartup(0x0202, &wsaData) == 0);
}
Socket_context::~Socket_context() {
	WSACleanup();
}

/**
 * Helper, does some casting and asserting
 */
SOCKET& get_sock(Socket const& sock) {
	static_assert(sizeof(sock.data) >= sizeof(SOCKET),
				  "sock.data is not big enough");
	return *((SOCKET*)sock.data);
}

// see header
Socket::Socket(Buffer_view address, Buffer_view port) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    auto code = getaddrinfo(address.c_str(), port.c_str(), &hints, &result);
    if (code) return;

	addrinfo* ptr = result;
	while (ptr) {
		auto sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
		if (sock == INVALID_SOCKET) {
			jerr << "Warning: Error at socket(): " << WSAGetLastError() << '\n';
		} else {
			auto code = connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen);
			if (code == SOCKET_ERROR) {
				closesocket(sock);
				jerr << "Warning: Unable to connect to server!\n";
			} else {
				get_sock(*this) = sock;
				break;
			}
		}
		
		ptr = ptr->ai_next;
	}
	
	freeaddrinfo(result);
	if (not ptr) return;

	initialized = true;
}

// see header
void Socket::close() {
	if (!initialized) return;
	closesocket(get_sock(*this));
	initialized = false;
}

// see header
void Socket::send(Buffer_view buf) {
	assert(initialized);
	
	auto code = ::send(get_sock(*this), buf.data(), buf.size(), 0);
	if (code == SOCKET_ERROR) {
		jerr << "Warning: send failed: " << WSAGetLastError() << '\n';
		close();
	}
}

// see header
int Socket::recv(Buffer* into) {
	assert(into);
	assert(initialized);

	int total_count = 0;
	while(true) {
		into->reserve_space(256);
		auto result = ::recv(get_sock(*this), into->end(), into->space(), 0);
		
		if (result < 0) {
			jerr << "Warning: recv failed: " << WSAGetLastError() << '\n';
			close();
			return total_count;
		} else if (result < into->space()) {
			into->addsize(result);
			total_count += result;
			return total_count;
		} else if (result == into->space()) {
			into->addsize(result);
			total_count += result;
		} else {
			assert(false);
		}
	}
}

} /* end of namespace jup */