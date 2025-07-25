#ifndef HEADER_CURL_WARNLESS_H
#define HEADER_CURL_WARNLESS_H
/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/

#include "../curl_setup.h"

#ifdef USE_WINSOCK
#include <curl/curl.h> /* for curl_socket_t */
#endif

#define CURLX_FUNCTION_CAST(target_type, func) \
  (target_type)(void (*) (void))(func)

unsigned char curlx_ultouc(unsigned long ulnum);

int curlx_uztosi(size_t uznum);

unsigned long curlx_uztoul(size_t uznum);

unsigned int curlx_uztoui(size_t uznum);

int curlx_sltosi(long slnum);

unsigned int curlx_sltoui(long slnum);

unsigned short curlx_sltous(long slnum);

ssize_t curlx_uztosz(size_t uznum);

size_t curlx_sotouz(curl_off_t sonum);

int curlx_sztosi(ssize_t sznum);

unsigned short curlx_uitous(unsigned int uinum);

size_t curlx_sitouz(int sinum);

#ifdef _WIN32
#undef  read
#define read(fd, buf, count)  (ssize_t)_read(fd, buf, curlx_uztoui(count))
#undef  write
#define write(fd, buf, count) (ssize_t)_write(fd, buf, curlx_uztoui(count))
#endif

#endif /* HEADER_CURL_WARNLESS_H */
