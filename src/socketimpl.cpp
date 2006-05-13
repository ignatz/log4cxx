/*
 * Copyright 2003,2004 The Apache Software Foundation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License. may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <log4cxx/portability.h>

#include <log4cxx/helpers/socketimpl.h>
#include <log4cxx/helpers/loglog.h>
#include <log4cxx/helpers/stringhelper.h>
#include <log4cxx/helpers/pool.h>
#include <log4cxx/helpers/transcoder.h>

#include "apr_network_io.h"
#include "apr_lib.h"


using namespace log4cxx;
using namespace log4cxx::helpers;

IMPLEMENT_LOG4CXX_OBJECT(SocketImpl)

#include <string.h>
#include <assert.h>

#include <apr_support.h>
#include <apr_signal.h>


SocketException::SocketException() : errorNumber(0), msg("SocketException") {
}

SocketException::SocketException(const char *what, apr_status_t status) :
  errorNumber(status) {

  // build the error message text
  char buffer[200];
  apr_strerror(status, buffer, sizeof buffer);
  msg = std::string(what) + std::string(": ") + buffer;
}

SocketException::SocketException(apr_status_t status) :
  errorNumber(status) {

  // build the error message text
  char buffer[200];
  apr_strerror(status, buffer, sizeof buffer);
  msg = std::string("SocketException: ") + buffer;
}

SocketException::SocketException(const SocketException& src)
   : IOException(src), errorNumber(src.getErrorNumber()) {
}

SocketException::~SocketException() throw() {
}

const char* SocketException::what() const throw() {
   return msg.c_str();
}

apr_status_t SocketException::getErrorNumber() const {
  return errorNumber;
}

PlatformSocketException::PlatformSocketException(const char* what, apr_status_t status) :
   SocketException(what, status) {
}

PlatformSocketException::PlatformSocketException(const PlatformSocketException& src)
   : SocketException(src) {
}

PlatformSocketException::~PlatformSocketException() throw() {
}

ConnectException::ConnectException()
    : PlatformSocketException("ConnectException", 0) {
}

ConnectException::ConnectException(apr_status_t status) 
   : PlatformSocketException("ConnectException", status) {
}

ConnectException::ConnectException(const ConnectException& src)
   : PlatformSocketException(src) {
}

ConnectException::~ConnectException() throw() {
}

BindException::BindException()
    : PlatformSocketException("BindException", 0) {
}

BindException::BindException(apr_status_t status) 
   : PlatformSocketException("BindException", status) {
}

BindException::BindException(const BindException& src)
   : PlatformSocketException(src) {
}

BindException::~BindException() throw() {
}

InterruptedIOException::InterruptedIOException() {
}

InterruptedIOException::InterruptedIOException(const InterruptedIOException& src)
   : IOException(src) {
}

InterruptedIOException::~InterruptedIOException() throw() {
}


const char* InterruptedIOException::what() const throw() {
   return "Interrupted IO exception";
}

SocketTimeoutException::SocketTimeoutException() {
}

SocketTimeoutException::SocketTimeoutException(const SocketTimeoutException& src)
   : InterruptedIOException(src) {
}

SocketTimeoutException::~SocketTimeoutException() throw() {
}


const char* SocketTimeoutException::what() const throw() {
   return "Socket timeout exception";
}


SocketImpl::SocketImpl() : address(), socket(0), localport(-1), port(0)
{
}

SocketImpl::~SocketImpl()
{
        try
        {
                close();
        }
        catch(SocketException&)
        {
        }
}

/** Accepts a connection. */
void SocketImpl::accept(SocketImplPtr s)
{
      // If a timeout is set then wait at most for the specified timeout
      if (getSoTimeout() > 0) {
        apr_status_t status = apr_wait_for_io_or_timeout(NULL, (apr_socket_t*) socket, 0);
        if (status == APR_TIMEUP) {
          throw SocketTimeoutException();
        }
        if (status != APR_SUCCESS) {
          throw SocketException(status);
        }
      }

      // Accept new connection
      apr_socket_t *clientSocket = 0;
      apr_status_t status = 
          apr_socket_accept(&clientSocket, (apr_socket_t*) socket, (apr_pool_t*) s->memoryPool.getAPRPool());
      if (status != APR_SUCCESS) {
        throw SocketException(status);
      }

      // get client socket address
      apr_sockaddr_t *client_addr;
      status = apr_socket_addr_get(&client_addr, APR_REMOTE, clientSocket);
      if (status != APR_SUCCESS) {
        throw SocketException(status);
      }

      // retrieve the IP address from the client socket's apr_sockaddr_t
      LogString ipAddrString;
      char *ipAddr;
      apr_sockaddr_ip_get(&ipAddr, client_addr);
      Transcoder::decode(ipAddr, strlen(ipAddr), ipAddrString);

      // retrieve the host name from the client socket's apr_sockaddr_t
      LogString hostNameString;
      char *hostName;
      apr_getnameinfo(&hostName, client_addr, 0);
      Transcoder::decode(hostName, strlen(hostName), hostNameString);

      s->address = new InetAddress(hostNameString, ipAddrString);
      s->socket = clientSocket;
      s->port = client_addr->port;
}

/** Returns the number of bytes that can be read from this socket
without blocking.
*/
int SocketImpl::available()
{
        // TODO
        return 0;
}

/** Binds this socket to the specified port number
on the specified host.
*/
void SocketImpl::bind(InetAddressPtr address, int port)
{
        LOG4CXX_ENCODE_CHAR(host, address->getHostAddress());

        // Create server socket address (including port number)
        apr_sockaddr_t *server_addr;
        apr_status_t status = 
            apr_sockaddr_info_get(&server_addr, host.c_str(), APR_INET,
                                  port, 0, (apr_pool_t*) memoryPool.getAPRPool());
        if (status != APR_SUCCESS) {
          throw ConnectException(status);
        }

        // bind the socket to the address
        status = apr_socket_bind((apr_socket_t*) socket, server_addr);
        if (status != APR_SUCCESS) {
          throw BindException(status);
        }

        this->localport = port;
}

/** Closes this socket. */
void SocketImpl::close()
{
        if (socket != 0) {
          LOGLOG_DEBUG(LOG4CXX_STR("closing socket"));
          apr_status_t status = apr_socket_close((apr_socket_t*) socket);
          if (status != APR_SUCCESS) {
            throw SocketException(status);
          }

          address = 0;
          socket = 0;
          port = 0;
          localport = -1;
        }
}

/**  Connects this socket to the specified port number
on the specified host.
*/
void SocketImpl::connect(InetAddressPtr address, int port)
{
        LOG4CXX_ENCODE_CHAR(host, address->getHostAddress());

        // create socket address (including port)
        apr_sockaddr_t *client_addr;
        apr_status_t status = 
            apr_sockaddr_info_get(&client_addr, host.c_str(), APR_INET,
                                  port, 0, (apr_pool_t*) memoryPool.getAPRPool());
        if (status != APR_SUCCESS) {
          throw ConnectException(status);
        }

        // connect the socket
        status =  apr_socket_connect((apr_socket_t*) socket, client_addr);
        if (status != APR_SUCCESS) {
          throw ConnectException();
        }

        this->address = address;
        this->port = port;
}

/** Connects this socket to the specified port on the named host. */
void SocketImpl::connect(const LogString& host, int port)
{
        connect(InetAddress::getByName(host), port);
}

/** Creates either a stream or a datagram socket. */
void SocketImpl::create(bool stream)
{
  apr_socket_t* newSocket;
  apr_status_t status =
    apr_socket_create(&newSocket, APR_INET, stream ? SOCK_STREAM : SOCK_DGRAM, 
                      APR_PROTO_TCP, (apr_pool_t*) memoryPool.getAPRPool());
  socket = newSocket;
  if (status != APR_SUCCESS) {
    throw SocketException(status);
  }
}

/** Sets the maximum queue length for incoming connection
indications (a request to connect) to the count argument.
*/
void SocketImpl::listen(int backlog)
{

  apr_status_t status = apr_socket_listen ((apr_socket_t*) socket, backlog);
  if (status != APR_SUCCESS) {
    throw SocketException(status);
  }
}

/** Returns the address and port of this socket as a String.
*/
LogString SocketImpl::toString() const
{
        LogString oss(address->getHostAddress());
        oss.append(1, LOG4CXX_STR(':'));
        Pool p;
        oss.append(StringHelper::toString(port, p));
        return oss;
}

// thanks to Yves Mettier (ymettier@libertysurf.fr) for this routine
size_t SocketImpl::read(void * buf, size_t len) const
{

// LOGLOG_DEBUG(LOG4CXX_STR("SocketImpl::reading ") << len << LOG4CXX_STR(" bytes."));
        char * p = (char *)buf;

        while ((size_t)(p - (char *)buf) < len)
        {
          apr_size_t len_read = len - (p - (const char *)buf);
          apr_status_t status = apr_socket_recv((apr_socket_t*) socket, p, &len_read);
          if (status != APR_SUCCESS) {
            throw SocketException(status);
          }
          if (len_read == 0) {
            break;
          }

          p += len_read;
        }

        return (p - (const char *)buf);
}

// thanks to Yves Mettier (ymettier@libertysurf.fr) for this routine
size_t SocketImpl::write(const void * buf, size_t len)
{
// LOGLOG_DEBUG(LOG4CXX_STR("SocketImpl::write ") << len << LOG4CXX_STR(" bytes."));

        const char * p = (const char *)buf;

        while ((size_t)(p - (const char *)buf) < len)
        {
          apr_size_t len_written = len - (p - (const char *)buf);

          // while writing to the socket, we need to ignore the SIGPIPE
          // signal. Otherwise, when the client has closed the connection,
          // the send() function would not return an error but call the
          // SIGPIPE handler.
#if APR_HAVE_SIGACTION
          apr_sigfunc_t* old = apr_signal(SIGPIPE, SIG_IGN);
          apr_status_t status = apr_socket_send((apr_socket_t*) socket, p, &len_written);
          apr_signal(SIGPIPE, old);
#else
          apr_status_t status = apr_socket_send((apr_socket_t*) socket, p, &len_written);
#endif

          if (status != APR_SUCCESS) {
            throw SocketException(status);
          }
          if (len_written == 0) {
            break;
          }

          p += len_written;
        }

        return (p - (const char *)buf);
}

/** Retrive setting for SO_TIMEOUT.
*/
int SocketImpl::getSoTimeout() const
{
  apr_interval_time_t timeout;
  apr_status_t status = apr_socket_timeout_get((apr_socket_t*) socket, &timeout);
  if (status != APR_SUCCESS) {
    throw SocketException(status);
  }

  return timeout / 1000;
}

/** Enable/disable SO_TIMEOUT with the specified timeout, in milliseconds.
*/
void SocketImpl::setSoTimeout(int timeout)
{
  apr_interval_time_t time = timeout * 1000;
  apr_status_t status = apr_socket_timeout_set((apr_socket_t*) socket, time);
  if (status != APR_SUCCESS) {
    throw SocketException(status);
  }
}
