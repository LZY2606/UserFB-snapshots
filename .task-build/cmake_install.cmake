# Install script for directory: /Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "RelWithDebInfo")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/workflow" TYPE FILE RENAME "workflow-config.cmake" FILES "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/.task-build/config.toinstall.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/workflow" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/.task-build/workflow-config-version.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/workflow" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/ProtocolMessage.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/http_parser.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/HttpMessage.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/HttpUtil.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/redis_parser.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/RedisMessage.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/mysql_stream.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/MySQLMessage.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/MySQLMessage.inl"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/MySQLResult.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/MySQLResult.inl"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/MySQLUtil.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/mysql_parser.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/mysql_types.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/mysql_byteorder.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/PackageWrapper.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/SSLWrapper.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/dns_types.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/dns_parser.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/DnsMessage.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/DnsUtil.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/TLVMessage.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/protocol/ConsulDataTypes.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/server/WFServer.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/server/WFDnsServer.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/server/WFHttpServer.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/server/WFRedisServer.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/server/WFMySQLServer.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/client/WFHttpChunkedClient.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/client/WFMySQLConnection.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/client/WFRedisSubscriber.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/client/WFConsulClient.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/client/WFDnsClient.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/manager/DnsCache.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/manager/WFGlobal.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/manager/UpstreamManager.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/manager/RouteManager.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/manager/EndpointParams.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/manager/WFFuture.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/manager/WFFacilities.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/manager/WFFacilities.inl"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/util/json_parser.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/util/EncodeStream.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/util/LRUCache.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/util/StringUtil.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/util/URIParser.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFConnection.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFTask.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFTask.inl"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFGraphTask.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFTaskError.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFTaskFactory.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFTaskFactory.inl"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFAlgoTaskFactory.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFAlgoTaskFactory.inl"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/Workflow.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFOperator.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFResourcePool.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/WFMessageQueue.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/HttpTaskImpl.inl"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/factory/RedisTaskImpl.inl"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/nameservice/WFNameService.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/nameservice/WFDnsResolver.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/nameservice/WFServiceGovernance.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/nameservice/UpstreamPolicies.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/CommRequest.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/CommScheduler.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/Communicator.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/SleepRequest.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/ExecRequest.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/IORequest.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/Executor.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/list.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/mpoller.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/poller.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/msgqueue.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/rbtree.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/SubTask.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/thrdpool.h"
    "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/src/kernel/IOService_thread.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/workflow-1.1.0" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/README.md")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/.task-build/src/cmake_install.cmake")

endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/.task-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/X1196/attempts/20261113T164155251717Z/repo/.task-build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
