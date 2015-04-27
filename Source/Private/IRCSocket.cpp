/*
 * Copyright (C) 2011 Fredi Machado <https://github.com/Fredi>
 * IRCClient is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * http://www.gnu.org/licenses/lgpl.html 
 */

#include "TwitchHype.h"

#include <cstring>
#include <fcntl.h>
#include "IRCSocket.h"

#define MAXDATASIZE 4096

bool IRCSocket::Init()
{
    #ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData))
	{
		UE_LOG(LogUTTwitchHype, Warning, TEXT("Unable to initialize Winsock."));
        return false;
    }
    #endif

    if ((_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
	{
		UE_LOG(LogUTTwitchHype, Warning, TEXT("Socket error."));
        #ifdef _WIN32
        WSACleanup();
        #endif
        return false;
    }

    int on = 1;
    if (setsockopt(_socket, SOL_SOCKET, SO_REUSEADDR, (char const*)&on, sizeof(on)) == -1)
	{
		UE_LOG(LogUTTwitchHype, Warning, TEXT("Invalid socket."));
        #ifdef _WIN32
        WSACleanup();
        #endif
        return false;
    }

    #ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(_socket, FIONBIO, &mode);
    #else
    fcntl(_socket, F_SETFL, O_NONBLOCK);
    fcntl(_socket, F_SETFL, O_ASYNC);
    #endif

    return true;
}

bool IRCSocket::Connect(char const* host, int port)
{
    hostent* he;
	he = gethostbyname(host);
    if (!he)
	{
		UE_LOG(LogUTTwitchHype, Warning, TEXT("Could not resolve host: %s"), ANSI_TO_TCHAR(host));
        #ifdef _WIN32
        WSACleanup();
        #endif
        return false;
    }

    sockaddr_in addr;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = *((const in_addr*)he->h_addr);
    memset(&(addr.sin_zero), '\0', 8);


    if (connect(_socket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
	{
#ifdef _WIN32
		int error = WSAGetLastError();
#else
		int error = errno;
#endif

#ifdef _WIN32
		if (error == WSAEWOULDBLOCK)
#else
		if (error == EINPROGRESS)
#endif
		{
			_connecting = true;
		}
		else
		{
			UE_LOG(LogUTTwitchHype, Warning, TEXT("Could not connect to: %s"), ANSI_TO_TCHAR(host));
#ifdef _WIN32
#endif
			closesocket(_socket);
			return false;
		}
    }

    _connected = true;

    return true;
}

void IRCSocket::Disconnect()
{
    if (_connected)
    {
        #ifdef _WIN32
        shutdown(_socket, 2);
        #endif
        closesocket(_socket);
        _connected = false;
    }
	_connecting = false;
}

bool IRCSocket::SendData(char const* data)
{
    if (_connected)
        if (send(_socket, data, strlen(data), 0) == -1)
            return false;

    return true;
}

std::string IRCSocket::ReceiveData()
{
	if (_connecting)
	{

	}
    char buffer[MAXDATASIZE];

    memset(buffer, 0, MAXDATASIZE);

    int bytes = recv(_socket, buffer, MAXDATASIZE - 1, 0);

	if (bytes > 0)
	{
		return std::string(buffer);
	}

    return "";
}

void IRCSocket::CheckConnected()
{
	if (_connecting)
	{
		fd_set myset;
		struct timeval tv;      
		tv.tv_sec = 0;
		tv.tv_usec = 500; 
		FD_ZERO(&myset);
		FD_SET(_socket, &myset);
		int retval = select(_socket + 1, NULL, &myset, NULL, &tv);
		if (retval > 0)
		{
			_connecting = false;
		}
		else if (retval == SOCKET_ERROR)
		{
			// print the error here
			UE_LOG(LogUTTwitchHype, Warning, TEXT("select failed!"));

			_connecting = false;
			_connected = false;
		}
	}
}