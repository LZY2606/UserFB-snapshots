/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version. (See <http://www.gnu.org/copyleft/lesser.html>.)
**********/
// Copyright (c) 1996-2026 Live Networks, Inc.  All rights reserved.
//
// testTransportReconfiguration.cpp
//
// Minimal example of enabling in-place transport reconfiguration on a plaintext, unicast
// RTSPServer.  With the feature enabled, a client may re-issue SETUP on an already-established
// track to move it between unicast UDP and RTP/AVP/TCP interleaving (or to change the UDP port
// pair / interleaved channels) without the server pausing, deleting and recreating the stream.
//
// The media source, RTP sink and RTCP instance - and therefore SSRC, RTP sequence numbers, RTP
// timestamps and the accumulated RTCP statistics - are preserved across the switch.  No second
// PLAY is required; a track that is playing keeps flowing (to the new destination), and a paused
// track stays paused until the next PLAY.
//
// Build together with the live555 libraries, e.g. (GNU make, from the top of the source tree):
//   c++ -I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include \
//       -IBasicUsageEnvironment/include testProgs/testTransportReconfiguration.cpp \
//       liveMedia/libliveMedia.a groupsock/libgroupsock.a \
//       BasicUsageEnvironment/libBasicUsageEnvironment.a UsageEnvironment/libUsageEnvironment.a \
//       -o testTransportReconfiguration
//
// Then point any RTSP client at rtsp://<host>:8554/reconfigTest and, while playing, send another
// SETUP for the same track with a different Transport: header.  For example, with openRTSP:
//   openRTSP -t rtsp://127.0.0.1:8554/reconfigTest          # play over TCP interleaving
// then re-SETUP the same URL with "Transport: RTP/AVP;unicast;client_port=...." to move to UDP.

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

UsageEnvironment* env;

// A H.264 Elementary Stream video server media subsession would normally be created by a
// FileServerMediaSubsession helper.  Here we keep the example self-contained by reusing the
// generic H.264 file subsession type.
#include "H264VideoFileServerMediaSubsession.hh"
#include "H264VideoRTPSink.hh"
#include "H264VideoStreamFramer.hh"

class ReconfigH264Subsession : public H264VideoFileServerMediaSubsession {
public:
  static ReconfigH264Subsession* createNew(UsageEnvironment& env, char const* fileName,
                                           Boolean reuseFirstSource) {
    return new ReconfigH264Subsession(env, fileName, reuseFirstSource);
  }

protected:
  ReconfigH264Subsession(UsageEnvironment& env, char const* fileName, Boolean reuseFirstSource)
    : H264VideoFileServerMediaSubsession(env, fileName, reuseFirstSource) {}
  // No override needed: H264VideoFileServerMediaSubsession derives (transitively) from
  // OnDemandServerMediaSubsession, whose supportsTransportReconfiguration() returns True by
  // default, so the feature applies automatically once it is enabled on the server.
};

int main(int /*argc*/, char** /*argv*/) {
  // Begin by setting up our usage environment:
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  env = BasicUsageEnvironment::createNew(*scheduler);

  UserAuthenticationDatabase* authDB = NULL;
  // To implement digest authentication, create a "UserAuthenticationDatabase" here.

  RTSPServer* rtspServer = RTSPServer::createNew(*env, 8554, authDB);
  if (rtspServer == NULL) {
    *env << "Failed to create RTSP server: " << env->getResultMsg() << "\n";
    exit(1);
  }

  // *** The single line that turns on in-place transport reconfiguration. ***
  // It must be set before the server accepts clients.  The default is disabled, in which case a
  // re-SETUP keeps the historical pause/delete/recreate behavior exactly.
  rtspServer->enableTransportReconfiguration(True);

  char const* descriptionString = "Session demonstrating in-place transport reconfiguration";
  char const* fileName = "test.264"; // replace with a real H.264 Elementary Stream file

  ServerMediaSession* sms
    = ServerMediaSession::createNew(*env, "reconfigTest", fileName, descriptionString);
  sms->addSubsession(ReconfigH264Subsession::createNew(*env, fileName, False/*reuseFirstSource*/));
  rtspServer->addServerMediaSession(sms);

  char* url = rtspServer->rtspURL(sms);
  *env << "Play this stream using the URL \"" << url << "\"\n"
       << "Re-issue SETUP on the same track while playing to switch transport in place.\n";
  delete[] url;

  // Run the event loop forever (the server handles clients internally):
  env->taskScheduler().doEventLoop(); // does not return

  return 0; // only to prevent compiler warning
}
