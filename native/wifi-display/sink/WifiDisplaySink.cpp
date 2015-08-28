
//#define LOG_NDEBUG 0
#define LOG_TAG "WifiDisplaySink"
#include <utils/Log.h>

#include "WifiDisplaySink.h"
#include "ParsedMessage.h"
#include "RTPSink.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>

namespace android {

WifiDisplaySink::WifiDisplaySink(
        const sp<ANetworkSession> &netSession,
        const sp<ISurfaceTexture> &surfaceTex)
    : mState(UNDEFINED),
      mNetSession(netSession),
      mSurfaceTex(surfaceTex),
      mSessionID(0),
      mNextCSeq(1) {
}

WifiDisplaySink::~WifiDisplaySink() {
}

void WifiDisplaySink::start(const char *sourceHost, int32_t sourcePort) {
    sp<AMessage> msg = new AMessage(kWhatStart, id());
    msg->setString("sourceHost", sourceHost);
    msg->setInt32("sourcePort", sourcePort);
    msg->post();
}

void WifiDisplaySink::start(const char *uri) {
    sp<AMessage> msg = new AMessage(kWhatStart, id());
    msg->setString("setupURI", uri);
    msg->post();
}

// static
bool WifiDisplaySink::ParseURL(
        const char *url, AString *host, int32_t *port, AString *path,
        AString *user, AString *pass) {
    host->clear();
    *port = 0;
    path->clear();
    user->clear();
    pass->clear();

    if (strncasecmp("rtsp://", url, 7)) {
        return false;
    }

    const char *slashPos = strchr(&url[7], '/');

    if (slashPos == NULL) {
        host->setTo(&url[7]);
        path->setTo("/");
    } else {
        host->setTo(&url[7], slashPos - &url[7]);
        path->setTo(slashPos);
    }

    ssize_t atPos = host->find("@");

    if (atPos >= 0) {
        // Split of user:pass@ from hostname.

        AString userPass(*host, 0, atPos);
        host->erase(0, atPos + 1);

        ssize_t colonPos = userPass.find(":");

        if (colonPos < 0) {
            *user = userPass;
        } else {
            user->setTo(userPass, 0, colonPos);
            pass->setTo(userPass, colonPos + 1, userPass.size() - colonPos - 1);
        }
    }

    const char *colonPos = strchr(host->c_str(), ':');

    if (colonPos != NULL) {
        char *end;
        unsigned long x = strtoul(colonPos + 1, &end, 10);

        if (end == colonPos + 1 || *end != '\0' || x >= 65536) {
            return false;
        }

        *port = x;

        size_t colonOffset = colonPos - host->c_str();
        size_t trailing = host->size() - colonOffset;
        host->erase(colonOffset, trailing);
    } else {
        *port = 554;
    }

    return true;
}

//������Դ�ˣ�source����Ϣ
void WifiDisplaySink::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatStart:       //���Կ�ʼ�Ľ׶�
        {
            int32_t sourcePort;

            if (msg->findString("setupURI", &mSetupURI)) {
                AString path, user, pass;
                CHECK(ParseURL(          //��URL�н�����Host��Port
                            mSetupURI.c_str(),
                            &mRTSPHost, &sourcePort, &path, &user, &pass)
                        && user.empty() && pass.empty());
            } else {
                CHECK(msg->findString("sourceHost", &mRTSPHost));
                CHECK(msg->findInt32("sourcePort", &sourcePort));
            }

            sp<AMessage> notify = new AMessage(kWhatRTSPNotify, id());   //��ϢЭ��

            status_t err = mNetSession->createRTSPClient(
                    mRTSPHost.c_str(), sourcePort, notify, &mSessionID);//����RTSP�ͻ���
            CHECK_EQ(err, (status_t)OK);

            mState = CONNECTING;
            break;
        }

        case kWhatRTSPNotify:              //RTSP����Э��
        {
            int32_t reason;
            CHECK(msg->findInt32("reason", &reason));

            switch (reason) {
                case ANetworkSession::kWhatError:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    AString detail;
                    CHECK(msg->findString("detail", &detail));

                    ALOGE("An error occurred in session %d (%d, '%s/%s').",
                          sessionID,
                          err,
                          detail.c_str(),
                          strerror(-err));

                    if (sessionID == mSessionID) {
                        ALOGI("Lost control connection.");

                        // The control connection is dead now.
                        mNetSession->destroySession(mSessionID);
                        mSessionID = 0;

                        looper()->stop();
                    }
                    break;
                }

                case ANetworkSession::kWhatConnected:
                {
                    ALOGI("We're now connected.");
                    mState = CONNECTED;

                    if (!mSetupURI.empty()) {
                        status_t err =
                            sendDescribe(mSessionID, mSetupURI.c_str());

                        CHECK_EQ(err, (status_t)OK);
                    }
                    break;
                }

                case ANetworkSession::kWhatData:
                {
                    onReceiveClientData(msg);  //������Ϣ������  
                    break;
                }

                case ANetworkSession::kWhatBinaryData:
                {
                    CHECK(sUseTCPInterleaving);

                    int32_t channel;
                    CHECK(msg->findInt32("channel", &channel));

                    sp<ABuffer> data;
                    CHECK(msg->findBuffer("data", &data));

                    mRTPSink->injectPacket(channel == 0 /* isRTP */, data);
                    break;
                }

                default:
                    TRESPASS();
            }
            break;
        }

        case kWhatStop:
        {
            looper()->stop();
            break;
        }

        default:
            TRESPASS();
    }
}

void WifiDisplaySink::registerResponseHandler(
        int32_t sessionID, int32_t cseq, HandleRTSPResponseFunc func) {
    ResponseID id;
    id.mSessionID = sessionID;
    id.mCSeq = cseq;
    mResponseHandlers.add(id, func);
}

status_t WifiDisplaySink::sendM2(int32_t sessionID) {
    ALOGD("WifiDisplaySink call sendM2");
    AString request = "OPTIONS * RTSP/1.0\r\n";
    AppendCommonResponse(&request, mNextCSeq);

    request.append(
            "Require: org.wfa.wfd1.0\r\n"
            "\r\n");

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySink::onReceiveM2Response);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySink::onReceiveM2Response(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    ALOGD("WifiDisplaySink onReceiveM2Response");
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    return OK;
}

status_t WifiDisplaySink::onReceiveDescribeResponse(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    ALOGD("WifiDisplaySink onReceiveDescribeResponse");
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    return sendSetup(sessionID, mSetupURI.c_str());
}

// on receive M6 response.
status_t WifiDisplaySink::onReceiveSetupResponse(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    ALOGD("WifiDisplaySink onReceiveSetupResponse");
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    if (!msg->findString("session", &mPlaybackSessionID)) {
        return ERROR_MALFORMED;
    }

    if (!ParsedMessage::GetInt32Attribute(
                mPlaybackSessionID.c_str(),
                "timeout",
                &mPlaybackSessionTimeoutSecs)) {
        mPlaybackSessionTimeoutSecs = -1;
    }

    ssize_t colonPos = mPlaybackSessionID.find(";");
    if (colonPos >= 0) {
        // Strip any options from the returned session id.
        mPlaybackSessionID.erase(
                colonPos, mPlaybackSessionID.size() - colonPos);
    }

    status_t err = configureTransport(msg);

    if (err != OK) {
        return err;
    }

    mState = PAUSED;

    AString playCommand = StringPrintf("rtsp://%s/wfd1.0/streamid=0", mPresentation_URL.c_str());
    return sendPlay(
            sessionID,
            !mSetupURI.empty()
                ? mSetupURI.c_str() : playCommand.c_str());
}

status_t WifiDisplaySink::configureTransport(const sp<ParsedMessage> &msg) {
    ALOGD("WifiDisplaySink configureTransport");
    if (sUseTCPInterleaving) {
        return OK;
    }

    AString transport;
    if (!msg->findString("transport", &transport)) {
        ALOGE("Missing 'transport' field in SETUP response.");
        return ERROR_MALFORMED;
    }

    AString sourceHost;
    if (!ParsedMessage::GetAttribute(
                transport.c_str(), "source", &sourceHost)) {
        sourceHost = mRTSPHost;
    }

    int rtpPort, rtcpPort;

    AString serverPortStr;
    if (ParsedMessage::GetAttribute(
                transport.c_str(), "server_port", &serverPortStr)) {
        if (sscanf(serverPortStr.c_str(), "%d-%d", &rtpPort, &rtcpPort) == 2) {
            if (rtpPort <= 0 || rtpPort > 65535
                    || rtcpPort <=0 || rtcpPort > 65535
                    || rtcpPort != rtpPort + 1) {
                ALOGE("Invalid server_port description '%s'.",
                      serverPortStr.c_str());

                return ERROR_MALFORMED;
            }

            if (rtpPort & 1) {
                ALOGW("Server picked an odd numbered RTP port.");
            }
        } else if (sscanf(serverPortStr.c_str(), "%d", &rtpPort) == 1) {
            rtcpPort = rtpPort + 1;
        } else {
            ALOGE("Invalid server_port description '%s'.",
                  serverPortStr.c_str());
            return ERROR_MALFORMED;
        }

    } else {
        // ALOGI("Missing 'server_port' in Transport field. using default port");
        // rtpPort = 33633;
        // rtcpPort = 33634;

        ALOGI("Missing 'server_port' in Transport field. so not link to source.(RTP)");
        return OK;
    }

    return OK;  // Now, we not care the "server_port" parameter.
    // return mRTPSink->connect(sourceHost.c_str(), rtpPort, rtcpPort);
}

status_t WifiDisplaySink::onReceivePlayResponse(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    ALOGD("WifiDisplaySink onReceivePlayResponse");
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    mState = PLAYING;

    return OK;
}

void WifiDisplaySink::onReceiveClientData(const sp<AMessage> &msg) {
    ALOGD("WifiDisplaySink onReceiveClientData");

    int32_t sessionID;
    CHECK(msg->findInt32("sessionID", &sessionID));//��÷�����Ϣ��sessionID

    sp<RefBase> obj;
    CHECK(msg->findObject("data", &obj));//��÷�����Ϣ��object����

    sp<ParsedMessage> data =
        static_cast<ParsedMessage *>(obj.get());//����object�����ȡ�����������

    ALOGV("session %d received '%s'",
          sessionID, data->debugString().c_str());

    AString method;
    AString uri;
    data->getRequestField(0, &method);

    int32_t cseq;
    if (!data->findInt32("cseq", &cseq)) {  //��ȡsendʱ�������Ϣ���
        sendErrorResponse(sessionID, "400 Bad Request", -1 /* cseq */);
        return;
    }

    if (method.startsWith("RTSP/")) {  //�����Ϣ��"RTSP/"��ͷ
        // This is a response.

        ResponseID id;
        id.mSessionID = sessionID;
        id.mCSeq = cseq;

		//���� ResponseID��ȡע��ʱ��mResponseHandlers��KeyedVector<ResponseID, HandleRTSPResponseFunc>������ݽṹ�е�λ��
        ssize_t index = mResponseHandlers.indexOfKey(id);

        if (index < 0) {
            ALOGW("Received unsolicited server response, cseq %d", cseq);
            return;
        }

        HandleRTSPResponseFunc func = mResponseHandlers.valueAt(index); //����λ�û�ȡע��Ļظ�����  
        mResponseHandlers.removeItemsAt(index);

        status_t err = (this->*func)(sessionID, data);//��亯��ָ��  
         //�жϻظ����Ƿ��д�����Ϣ  
        CHECK_EQ(err, (status_t)OK);
    } else {
        AString version;
        data->getRequestField(2, &version);
        if (!(version == AString("RTSP/1.0"))) { //�ж�RTSPЭ��汾�Ƿ���ȷ
            sendErrorResponse(sessionID, "505 RTSP Version not supported", cseq);
            return;
        }

        if (method == "OPTIONS") {
            onOptionsRequest(sessionID, cseq, data);
        } else if (method == "GET_PARAMETER") {
            onGetParameterRequest(sessionID, cseq, data);
        } else if (method == "SET_PARAMETER") {
            onSetParameterRequest(sessionID, cseq, data);
        } else {
            sendErrorResponse(sessionID, "405 Method Not Allowed", cseq);
        }
    }
}

void WifiDisplaySink::onOptionsRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    ALOGD("WifiDisplaySink:: onOptionsRequest");
    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);
    response.append("Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER\r\n");
    response.append("\r\n");

	//֧��GET_PARAMETER, SET_PARAMETER�ķ���������ANetworkSession
    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);

    err = sendM2(sessionID);
    CHECK_EQ(err, (status_t)OK);
}

// on receive M3
void WifiDisplaySink::onGetParameterRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    ALOGD("WifiDisplaySink:: onGetParameterRequest");

    {
        // mRTPSink....
    }

    /* AString body =
        "wfd_video_formats: xxx\r\n"
        "wfd_audio_codecs: xxx\r\n"
        "wfd_client_rtp_ports: RTP/AVP/UDP;unicast xxx 0 mode=play\r\n"; */

    AString body =
        "wfd_video_formats: 48 00 02 02 0001DEFF 157C7FFF 00000FFF 00 0000 0000 00 none none\r\n"
        "wfd_audio_codecs: LPCM 00000003 00\r\n";

    body.append("wfd_content_protection: none\r\n");
    body.append("wfd_coupled_sink: 00 none\r\n");
    body.append("wfd_uibc_capability: none\r\n");
    body.append("wfd_standby_resume_capability: none\r\n");
    body.append("wfd_lg_dlna_uuid: none\r\n");
    //body.append("wfd_client_rtp_ports: RTP/AVP/UDP;unicast %d 0 mode=play\r\n",
    //            mRTPSink->getRTPPort());
    body.append("wfd_client_rtp_ports: RTP/AVP/UDP;unicast 15550 0 mode=play\r\n");
    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);
    response.append("Content-Type: text/parameters\r\n");
    response.append(StringPrintf("Content-Length: %d\r\n", body.size()));
    response.append("\r\n");
    response.append(body);

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

status_t WifiDisplaySink::sendDescribe(int32_t sessionID, const char *uri) {
    uri = "rtsp://xwgntvx.is.livestream-api.com/livestreamiphone/wgntv";
    uri = "rtsp://v2.cache6.c.youtube.com/video.3gp?cid=e101d4bf280055f9&fmt=18";

        ALOGD("WifiDisplaySink:: sendDescribe");

    AString request = StringPrintf("DESCRIBE %s RTSP/1.0\r\n", uri);
    AppendCommonResponse(&request, mNextCSeq);

    request.append("Accept: application/sdp\r\n");
    request.append("\r\n");

    status_t err = mNetSession->sendRequest(
            sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySink::onReceiveDescribeResponse);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySink::sendSetup(int32_t sessionID, const char *uri) {
    ALOGD("WifiDisplaySink:: sendSetup");

    mRTPSink = new RTPSink(mNetSession, mSurfaceTex);
    looper()->registerHandler(mRTPSink);

    status_t err = mRTPSink->init(sUseTCPInterleaving);

    if (err != OK) {
        looper()->unregisterHandler(mRTPSink->id());
        mRTPSink.clear();
        return err;
    }

    AString request = StringPrintf("SETUP %s RTSP/1.0\r\n", uri);

    AppendCommonResponse(&request, mNextCSeq);

    if (sUseTCPInterleaving) {
        request.append("Transport: RTP/AVP/TCP;interleaved=0-1\r\n");
    } else {
        int32_t rtpPort = mRTPSink->getRTPPort();

        request.append(
                StringPrintf(
                    "Transport: RTP/AVP/UDP;unicast;client_port=%d\r\n",
                    rtpPort));
    }

    request.append("\r\n");

    ALOGV("request = '%s'", request.c_str());

    err = mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySink::onReceiveSetupResponse);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySink::sendPlay(int32_t sessionID, const char *uri) {
    ALOGD("WifiDisplaySink: sendPlay");
    AString request = StringPrintf("PLAY %s RTSP/1.0\r\n", uri);

    AppendCommonResponse(&request, mNextCSeq);

    request.append(StringPrintf("Session: %s\r\n", mPlaybackSessionID.c_str()));
    request.append("\r\n");

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySink::onReceivePlayResponse);

    ++mNextCSeq;

    return OK;
}

// on receive M4, M5.
void WifiDisplaySink::onSetParameterRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    ALOGD("WifiDisplaySink onSetParameterRequest");
    const char *content = data->getContent();

    // if M4
    onSetParameterRequest_CheckM4Parameter(content);

    // if M5(setup) request.  then send M6
    if (strstr(content, "wfd_trigger_method: SETUP\r\n") != NULL) {
        // AString uri = StringPrintf("rtsp://%s/wfd1.0/streamid=0", mPresentation_URL.c_str());
        AString uri = StringPrintf("rtsp://%s/wfd1.0/streamid=0", mPresentation_URL.c_str());
        status_t err =
            sendSetup(
                    sessionID,
                    uri.c_str());

        CHECK_EQ(err, (status_t)OK);
    }

    // response this M*
    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);
    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

void WifiDisplaySink::onSetParameterRequest_CheckM4Parameter(const char *content) {
    ALOGV("onSetParameterRequest_CheckM4Parameter in");
    int len = strlen(content) + 1;
    char *contentTemp = new char[len];
    memset(contentTemp, 0, len);
    strcpy(contentTemp, content);
    contentTemp[len-1] = '\0';
    AString strOld(contentTemp);
    int pos1 = 0;
    int pos2 = 0;
    while (pos1 >= 0 && pos2 >= 0) {
        ALOGD("pos1 = %d, pos2 = %d", pos1, pos2);
        pos2 = strOld.find("\r\n", pos1);
        if (pos2 > 0) {
            char * array = new char[pos2 - pos1 + 1];
            memset(array, 0, pos2-pos1+1);
            strncpy(array, (strOld.c_str() + pos1), pos2 - pos1);
            AString oneLine = array;
            char tmp[20];
            memset(tmp, 20, 0);
            const char *startstr = "wfd_presentation_URL: rtsp://";
            int startstrlen = strlen(startstr);
            if (oneLine.startsWith(startstr)) {
                int endpos = oneLine.find("/", startstrlen);
                ALOGV("startlen = %d, endpos = %d", startstrlen, endpos);
                strncpy(tmp, oneLine.c_str() + startstrlen, endpos - startstrlen);
                tmp[endpos - startstrlen] = '\0';
                mPresentation_URL.clear();
                mPresentation_URL = tmp;
                break;
            }
        } else {
            break;
        }
        pos1 = pos2 + 2;
    }

    ALOGV("onSetParameterRequest_CheckM4Parameter result. mPresentation_URL = %s\n", mPresentation_URL.c_str());
}

void WifiDisplaySink::sendErrorResponse(
        int32_t sessionID,
        const char *errorDetail,
        int32_t cseq) {
    ALOGD("WifiDisplaySink sendErrorResponse");
    AString response;
    response.append("RTSP/1.0 ");
    response.append(errorDetail);
    response.append("\r\n");

    AppendCommonResponse(&response, cseq);

    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

// static
void WifiDisplaySink::AppendCommonResponse(AString *response, int32_t cseq) {
    time_t now = time(NULL);
    struct tm *now2 = gmtime(&now);
    char buf[128];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %z", now2);

    response->append("Date: ");
    response->append(buf);
    response->append("\r\n");

    response->append("User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n");

    if (cseq >= 0) {
        response->append(StringPrintf("CSeq: %d\r\n", cseq));
    }
}

}  // namespace android
