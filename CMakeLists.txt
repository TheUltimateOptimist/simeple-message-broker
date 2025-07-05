cmake_minimum_required(VERSION 3.10)
project(simple-message-broker C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

add_executable(smbbroker smbbroker.c)
add_executable(smbpublish smbpublish.c)
add_executable(smbsubscribe smbsubscribe.c)

