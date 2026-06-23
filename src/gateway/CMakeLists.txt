#/**
# * MaNGOS is a full featured server for World of Warcraft, supporting
# * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
# *
# * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
# *
# * This program is free software; you can redistribute it and/or modify
# * it under the terms of the GNU General Public License as published by
# * the Free Software Foundation; either version 2 of the License, or
# * (at your option) any later version.
# *
# * This program is distributed in the hope that it will be useful,
# * but WITHOUT ANY WARRANTY; without even the implied warranty of
# * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# * GNU General Public License for more details.
# *
# * You should have received a copy of the GNU General Public License
# * along with this program; if not, write to the Free Software
# * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
# *
# * World of Warcraft, and all World of Warcraft or Warcraft art, images,
# * and lore are copyrighted by Blizzard Entertainment, Inc.
# */

# Gateway source files (this directory only; auth/realm globs live in realmd)
file(GLOB SRC_GRP_GATEWAY *.cpp *.h)
source_group("Gateway" FILES ${SRC_GRP_GATEWAY})

configure_file(gateway.conf.dist.in ${CMAKE_CURRENT_BINARY_DIR}/gateway.conf.dist)

add_executable(gateway
    ${SRC_GRP_GATEWAY}
)

target_compile_features(gateway PUBLIC cxx_std_17)
set_target_properties(gateway PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(gateway
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
        ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(gateway
    PUBLIC
        shared
        Threads::Threads
        ${OPENSSL_LIBRARIES}
        OpenSSL::Crypto
        ace
)

install(
    TARGETS gateway
    DESTINATION ${BIN_DIR}
)

install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/gateway.conf.dist
    DESTINATION ${CONF_INSTALL_DIR}
)

# Standalone self-test for the auth digest (no DB / no socket). Pins the
# CMSG_AUTH_SESSION SHA-1 byte-assembly order.
add_executable(gateway_authtest
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test_gateway_auth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GatewayAuth.cpp
)

target_compile_features(gateway_authtest PUBLIC cxx_std_17)
set_target_properties(gateway_authtest PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(gateway_authtest
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(gateway_authtest
    PUBLIC
        shared
        Threads::Threads
        ${OPENSSL_LIBRARIES}
        OpenSSL::Crypto
        ace
)

# Standalone round-trip test for the GW_AC_EVENT payload (no DB / no socket).
# Pins the {clientId, acType, severity, detail} field order both the gateway
# (ReportAcViolation) and node (handleAcEvent) must agree on.
add_executable(gateway_aceventtest
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test_gateway_acevent.cpp
)

target_compile_features(gateway_aceventtest PUBLIC cxx_std_17)
set_target_properties(gateway_aceventtest PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(gateway_aceventtest
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(gateway_aceventtest
    PUBLIC
        shared
        Threads::Threads
        ${OPENSSL_LIBRARIES}
        OpenSSL::Crypto
        ace
)

if(WIN32 AND MSVC)
    install(
        FILES $<TARGET_PDB_FILE:gateway>
        DESTINATION ${BIN_DIR}
        OPTIONAL
        )
endif()
