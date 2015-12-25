/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Parts of this file is covered by:

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

 */

#include "config.h"

// This is the main source for the loolwsd program. LOOL uses several loolwsd processes: one main
// parent process that listens on the TCP port and accepts connections from LOOL clients, and a
// number of child processes, each which handles a viewing (editing) session for one document.

#include <errno.h>
#include <locale.h>
#include <pwd.h>
#include <unistd.h>

#ifdef __linux
#include <sys/capability.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#endif

#include <ftw.h>
#include <utime.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <mutex>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitInit.h>

#include <Poco/Exception.h>
#include <Poco/File.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/MessageHeader.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/PartHandler.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Path.h>
#include <Poco/Process.h>
#include <Poco/StringTokenizer.h>
#include <Poco/ThreadPool.h>
#include <Poco/Util/HelpFormatter.h>
#include <Poco/Util/Option.h>
#include <Poco/Util/OptionException.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Util/ServerApplication.h>
#include <Poco/Mutex.h>
#include <Poco/Net/DialogSocket.h>
#include <Poco/Net/Net.h>
#include <Poco/ThreadLocal.h>
#include <Poco/NamedMutex.h>
#include <Poco/FileStream.h>
#include <Poco/TemporaryFile.h>
#include <Poco/StreamCopier.h>
#include <Poco/URI.h>
#include <Poco/Environment.h>

#include "LOOLProtocol.hpp"
#include "LOOLSession.hpp"
#include "MasterProcessSession.hpp"
#include "ChildProcessSession.hpp"
#include "LOOLWSD.hpp"
#include "MessageQueue.hpp"
#include "Util.hpp"

using namespace LOOLProtocol;

using Poco::Exception;
using Poco::File;
using Poco::IOException;
using Poco::Net::HTTPClientSession;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPRequestHandlerFactory;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServer;
using Poco::Net::HTTPServerParams;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServerResponse;
using Poco::Net::ServerSocket;
using Poco::Net::SocketAddress;
using Poco::Net::WebSocket;
using Poco::Net::WebSocketException;
using Poco::Path;
using Poco::Process;
using Poco::Runnable;
using Poco::StringTokenizer;
using Poco::Thread;
using Poco::ThreadPool;
using Poco::Util::Application;
using Poco::Util::HelpFormatter;
using Poco::Util::IncompatibleOptionsException;
using Poco::Util::MissingOptionException;
using Poco::Util::Option;
using Poco::Util::OptionSet;
using Poco::Util::ServerApplication;
using Poco::Net::DialogSocket;
using Poco::FastMutex;
using Poco::Net::Socket;
using Poco::ThreadLocal;
using Poco::Random;
using Poco::NamedMutex;
using Poco::ProcessHandle;
using Poco::URI;

namespace
{
    void dropCapability(
#ifdef __linux
                        cap_value_t capability
#endif
                        )
    {
#ifdef __linux
        cap_t caps;
        cap_value_t cap_list[] = { capability };

        caps = cap_get_proc();
        if (caps == NULL)
        {
            Log::error("cap_get_proc() failed.");
            exit(1);
        }

        if (cap_set_flag(caps, CAP_EFFECTIVE, sizeof(cap_list)/sizeof(cap_list[0]), cap_list, CAP_CLEAR) == -1 ||
            cap_set_flag(caps, CAP_PERMITTED, sizeof(cap_list)/sizeof(cap_list[0]), cap_list, CAP_CLEAR) == -1)
        {
            Log::error("cap_set_flag() failed.");
            exit(1);
        }

        if (cap_set_proc(caps) == -1)
        {
            Log::error("cap_set_proc() failed.");
            exit(1);
        }

        char *capText = cap_to_text(caps, NULL);
        Log::info(std::string("Capabilities now: ") + capText);
        cap_free(capText);

        cap_free(caps);
#endif
        // We assume that on non-Linux we don't need to be root to be able to hardlink to files we
        // don't own, so drop root.
        if (geteuid() == 0 && getuid() != 0)
        {
            // The program is setuid root. Not normal on Linux where we use setcap, but if this
            // needs to run on non-Linux Unixes, setuid root is what it will bneed to be to be able
            // to do chroot().
            if (setuid(getuid()) != 0)
            {
                Log::error("setuid() failed.");
            }
        }
#if ENABLE_DEBUG
        if (geteuid() == 0 && getuid() == 0)
        {
#ifdef __linux
            // Argh, awful hack
            if (capability == CAP_FOWNER)
                return;
#endif

            // Running under sudo, probably because being debugged? Let's drop super-user rights.
            LOOLWSD::runningAsRoot = true;
            if (LOOLWSD::uid == 0)
            {
                struct passwd *nobody = getpwnam("nobody");
                if (nobody)
                    LOOLWSD::uid = nobody->pw_uid;
                else
                    LOOLWSD::uid = 65534;
            }
            if (setuid(LOOLWSD::uid) != 0)
            {
                Log::error("setuid() failed.");
            }
        }
#endif
    }
}

class QueueHandler: public Runnable
{
public:
    QueueHandler(MessageQueue& queue):
        _queue(queue)
    {
    }

    void setSession(std::shared_ptr<LOOLSession> session)
    {
        _session = session;
    }

    void run() override
    {
#ifdef __linux
        if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>("queue_handler"), 0, 0, 0) != 0)
            Log::error("Cannot set thread name.");
#endif

        while (true)
        {
            std::string input = _queue.get();
            if (input == "eof")
                break;
            if (!_session->handleInput(input.c_str(), input.size()))
                break;
        }
    }

private:
    std::shared_ptr<LOOLSession> _session;
    MessageQueue& _queue;
};

/// Handles the filename part of the convert-to POST request payload.
class ConvertToPartHandler : public Poco::Net::PartHandler
{
    std::string& _filename;
public:
    ConvertToPartHandler(std::string& filename)
        : _filename(filename)
    {
    }

    virtual void handlePart(const Poco::Net::MessageHeader& header, std::istream& stream) override
    {
        // Extract filename and put it to a temporary directory.
        std::string disp;
        Poco::Net::NameValueCollection params;
        if (header.has("Content-Disposition"))
        {
            std::string cd = header.get("Content-Disposition");
            Poco::Net::MessageHeader::splitParameters(cd, disp, params);
        }

        if (!params.has("filename"))
            return;

        Path tempPath = Path::forDirectory(Poco::TemporaryFile().tempName() + Path::separator());
        File(tempPath).createDirectories();
        tempPath.setFileName(params.get("filename"));
        _filename = tempPath.toString();

        // Copy the stream to _filename.
        std::ofstream fileStream;
        fileStream.open(_filename);
        Poco::StreamCopier::copyStream(stream, fileStream);
        fileStream.close();
    }
};

class RequestHandler: public HTTPRequestHandler
    /// Handle a WebSocket connection or a simple HTTP request.
{
public:
    RequestHandler()
    {
    }

    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override
    {
#ifdef __linux
        std::string thread_name;
        if (request.serverAddress().port() == LOOLWSD::MASTER_PORT_NUMBER)
            thread_name = "prision_socket";
        else
            thread_name = "client_socket";

        if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(thread_name.c_str()), 0, 0, 0) != 0)
            Log::error("Cannot set thread name.");
#endif

        if(!(request.find("Upgrade") != request.end() && Poco::icompare(request["Upgrade"], "websocket") == 0))
        {
            StringTokenizer tokens(request.getURI(), "/?");
            if (tokens.count() >= 2 && tokens[1] == "convert-to")
            {
                std::string fromPath;
                ConvertToPartHandler handler(fromPath);
                Poco::Net::HTMLForm form(request, request.stream(), handler);
                std::string format;
                if (form.has("format"))
                    format = form.get("format");

                if (!fromPath.empty() && !format.empty())
                {
                    // Load the document.
                    std::shared_ptr<WebSocket> ws;
                    LOOLSession::Kind kind = LOOLSession::Kind::ToClient;
                    std::shared_ptr<MasterProcessSession> session(new MasterProcessSession(ws, kind));
                    const std::string filePrefix("file://");
                    std::string load = "load url=" + filePrefix + fromPath;
                    session->handleInput(load.data(), load.size());

                    // Convert it to the requested format.
                    Path toPath(fromPath);
                    toPath.setExtension(format);
                    std::string toJailURL = filePrefix + LOOLSession::jailDocumentURL + Path::separator() + toPath.getFileName();
                    std::string saveas = "saveas url=" + toJailURL + " format=" + format + " options=";
                    session->handleInput(saveas.data(), saveas.size());
                    std::string toURL = session->getSaveAs();

                    // Send it back to the client.
                    std::string mimeType = "application/octet-stream";
                    if (toURL.find(filePrefix) == 0)
                        toURL = toURL.substr(filePrefix.length());
                    response.sendFile(toURL, mimeType);
                }
                else
                {
                    response.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
                    response.setContentLength(0);
                    response.send();
                }

                // Clean up the temporary directory the HTMLForm ctor created.
                Path tempDirectory(fromPath);
                tempDirectory.setFileName("");
                File(tempDirectory).remove(/*recursive=*/true);
            }
            else if (tokens.count() >= 2 && tokens[1] == "insertfile")
            {
                response.set("Access-Control-Allow-Origin", "*");
                response.set("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                response.set("Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");

                std::string tmpPath;
                ConvertToPartHandler handler(tmpPath);
                Poco::Net::HTMLForm form(request, request.stream(), handler);
                if (form.has("childid") && form.has("name"))
                {
                    std::string dirPath = LOOLWSD::childRoot + Path::separator() + form.get("childid") + LOOLSession::jailDocumentURL +
                        Path::separator() + "insertfile";
                    File(dirPath).createDirectory();
                    std::string fileName = dirPath + Path::separator() + form.get("name");
                    File(tmpPath).moveTo(fileName);

                    response.setStatus(HTTPResponse::HTTP_OK);
                    response.send();
                }
                else
                {
                    response.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
                    response.send();
                }
            }
            else if (tokens.count() >= 4)
            {
                // The user might request a file to download
                std::string dirPath = LOOLWSD::childRoot + "/" + tokens[1] + LOOLSession::jailDocumentURL + "/" + tokens[2];
                std::string fileName;
                URI::decode(tokens[3], fileName);
                std::string filePath = dirPath + "/" + fileName;
                std::cout << Util::logPrefix() << "HTTP request for: " << filePath << std::endl;
                File file(filePath);
                if (file.exists())
                {
                    response.set("Access-Control-Allow-Origin", "*");
                    Poco::Net::HTMLForm form(request);
                    std::string mimeType = "application/octet-stream";
                    if (form.has("mime_type"))
                        mimeType = form.get("mime_type");
                    response.sendFile(filePath, mimeType);
                    File dir(dirPath);
                    dir.remove(true);
                }
                else
                {
                    response.setStatus(HTTPResponse::HTTP_NOT_FOUND);
                    response.setContentLength(0);
                    response.send();
                }
            }
            else
            {
                response.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
                response.setContentLength(0);
                response.send();
            }
            return;
        }

        BasicTileQueue queue;
        Thread queueHandlerThread;
        QueueHandler handler(queue);
        Poco::Timespan waitTime(LOOLWSD::POLL_TIMEOUT);

        try
        {
            try
            {
                std::shared_ptr<WebSocket> ws(new WebSocket(request, response));

                LOOLSession::Kind kind;

                if (request.getURI() == LOOLWSD::CHILD_URI && request.serverAddress().port() == LOOLWSD::MASTER_PORT_NUMBER)
                    kind = LOOLSession::Kind::ToPrisoner;
                else
                    kind = LOOLSession::Kind::ToClient;

                std::shared_ptr<MasterProcessSession> session(new MasterProcessSession(ws, kind));

                // For ToClient sessions, we store incoming messages in a queue and have a separate
                // thread that handles them. This is so that we can empty the queue when we get a
                // "canceltiles" message.
                if (kind == LOOLSession::Kind::ToClient)
                {
                    handler.setSession(session);
                    queueHandlerThread.start(handler);
                }

                // Loop, receiving WebSocket messages either from the client, or from the child
                // process (to be forwarded to the client).
                int flags;
                int n;
                bool pollTimeout = true;
                ws->setReceiveTimeout(0);

                do
                {
                    char buffer[200000]; //FIXME: Dynamic?

                    if ((pollTimeout = ws->poll(waitTime, Socket::SELECT_READ)))
                    {
                        n = ws->receiveFrame(buffer, sizeof(buffer), flags);

                        if ((flags & WebSocket::FRAME_OP_BITMASK) == WebSocket::FRAME_OP_PONG)
                        {
                            n = 1;
                        }
                        else if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                        {
                            std::string firstLine = getFirstLine(buffer, n);
                            StringTokenizer tokens(firstLine, " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

                            if (kind == LOOLSession::Kind::ToClient && firstLine.size() == static_cast<std::string::size_type>(n))
                            {
                                queue.put(firstLine);
                            }
                            else
                            {
                                // Check if it is a "nextmessage:" and in that case read the large
                                // follow-up message separately, and handle that only.
                                int size;
                                if (tokens.count() == 2 && tokens[0] == "nextmessage:" && getTokenInteger(tokens[1], "size", size) && size > 0)
                                {
                                    char largeBuffer[size];

                                    n = ws->receiveFrame(largeBuffer, size, flags);
                                    if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                                    {
                                        if (!session->handleInput(largeBuffer, n))
                                            n = 0;
                                    }
                                }
                                else
                                {
                                    if (!session->handleInput(buffer, n))
                                        n = 0;
                                }
                            }
                        }
                    }
                }
                while (!LOOLWSD::isShutDown &&
                        (!pollTimeout || (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)));

                queue.clear();
                queue.put("eof");
                queueHandlerThread.join();
            }
            catch (WebSocketException& exc)
            {
                Log::error("RequestHandler::handleRequest(), WebSocketException: " + exc.message());
                switch (exc.code())
                {
                case WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
                    response.set("Sec-WebSocket-Version", WebSocket::WEBSOCKET_VERSION);
                    // fallthrough
                case WebSocket::WS_ERR_NO_HANDSHAKE:
                case WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
                case WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
                    response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
                    response.setContentLength(0);
                    response.send();
                    break;
                }
            }
        }
        catch (IOException& exc)
        {
            Log::error("IOException: " + exc.message());
        }
    }
};

class RequestHandlerFactory: public HTTPRequestHandlerFactory
{
public:
    RequestHandlerFactory()
    {
    }

    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest& request) override
    {
        std::string line = (Util::logPrefix() + "Request from " +
                            request.clientAddress().toString() + ": " +
                            request.getMethod() + " " +
                            request.getURI() + " " +
                            request.getVersion());

        for (HTTPServerRequest::ConstIterator it = request.begin(); it != request.end(); ++it)
        {
            line += " / " + it->first + ": " + it->second;
        }

        Log::info(line);
        return new RequestHandler();
    }
};

class TestOutput : public Runnable
{
public:
    TestOutput(WebSocket& ws) :
        _ws(ws)
    {
    }

    void run() override
    {
        int flags;
        int n;
        _ws.setReceiveTimeout(0);
        try
        {
            do
            {
                char buffer[200000];
                n = _ws.receiveFrame(buffer, sizeof(buffer), flags);

                if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                {
                    std::cout <<
                        Util::logPrefix() <<
                        "Client got " << n << " bytes: " << getAbbreviatedMessage(buffer, n) <<
                        std::endl;
                }
            }
            while (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
        }
        catch (WebSocketException& exc)
        {
            Log::error("TestOutput::run(), WebSocketException: " + exc.message());
            _ws.close();
        }
    }

private:
    WebSocket& _ws;
};

class TestInput : public Runnable
{
public:
    TestInput(ServerApplication& main, ServerSocket& svs, HTTPServer& srv) :
        _main(main),
        _svs(svs),
        _srv(srv)
    {
    }

    void run() override
    {
        HTTPClientSession cs("127.0.0.1", _svs.address().port());
        HTTPRequest request(HTTPRequest::HTTP_GET, "/ws");
        HTTPResponse response;
        WebSocket ws(cs, request, response);

        Thread thread;
        TestOutput output(ws);
        thread.start(output);

        if (isatty(0))
        {
            std::cout << std::endl;
            std::cout << "Enter LOOL WS requests, one per line. Enter EOF to finish." << std::endl;
        }

        while (!std::cin.eof())
        {
            std::string line;
            std::getline(std::cin, line);
            ws.sendFrame(line.c_str(), line.size());
        }
        thread.join();
        _srv.stopAll();
        _main.terminate();
    }

private:
    ServerApplication& _main;
    ServerSocket& _svs;
    HTTPServer& _srv;
};

int LOOLWSD::portNumber = DEFAULT_CLIENT_PORT_NUMBER;
int LOOLWSD::timeoutCounter = 0;
int LOOLWSD::writerBroker = -1;
Poco::UInt64 LOOLWSD::_childId = 0;
std::string LOOLWSD::cache = LOOLWSD_CACHEDIR;
std::string LOOLWSD::sysTemplate;
std::string LOOLWSD::loTemplate;
std::string LOOLWSD::childRoot;
std::string LOOLWSD::loSubPath = "lo";
std::string LOOLWSD::jail;
Poco::NamedMutex LOOLWSD::_namedMutexLOOL("loolwsd");

int LOOLWSD::_numPreSpawnedChildren = 10;
bool LOOLWSD::doTest = false;
volatile bool LOOLWSD::isShutDown = false;
#if ENABLE_DEBUG
bool LOOLWSD::runningAsRoot = false;
int LOOLWSD::uid = 0;
#endif
const std::string LOOLWSD::CHILD_URI = "/loolws/child/";
const std::string LOOLWSD::PIDLOG = "/tmp/loolwsd.pid";
const std::string LOOLWSD::LOKIT_PIDLOG = "/tmp/lokit.pid";
const std::string LOOLWSD::FIFO_FILE = "/tmp/loolwsdfifo";

LOOLWSD::LOOLWSD()
{
}

LOOLWSD::~LOOLWSD()
{
}

void LOOLWSD::handleSignal(int aSignal)
{
    std::cout << Util::logPrefix() << "Signal received: " << strsignal(aSignal) << std::endl;
    LOOLWSD::isShutDown = true;
}

void LOOLWSD::setSignals(bool isIgnored)
{
#ifdef __linux
    struct sigaction aSigAction;

    sigemptyset(&aSigAction.sa_mask);
    aSigAction.sa_flags = 0;
    aSigAction.sa_handler = (isIgnored ? SIG_IGN : handleSignal);

    sigaction(SIGTERM, &aSigAction, NULL);
    sigaction(SIGINT, &aSigAction, NULL);
    sigaction(SIGQUIT, &aSigAction, NULL);
    sigaction(SIGHUP, &aSigAction, NULL);
#endif
}

void LOOLWSD::initialize(Application& self)
{
    ServerApplication::initialize(self);
}

void LOOLWSD::uninitialize()
{
    ServerApplication::uninitialize();
}

void LOOLWSD::defineOptions(OptionSet& optionSet)
{
    ServerApplication::defineOptions(optionSet);

    optionSet.addOption(Option("help", "", "Display help information on command line arguments.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("port", "", "Port number to listen to (default: " + std::to_string(DEFAULT_CLIENT_PORT_NUMBER) + "),"
                             " must not be " + std::to_string(MASTER_PORT_NUMBER) + ".")
                        .required(false)
                        .repeatable(false)
                        .argument("port number"));

    optionSet.addOption(Option("cache", "", "Path to a directory where to keep the persistent tile cache (default: " + std::string(LOOLWSD_CACHEDIR) + ").")
                        .required(false)
                        .repeatable(false)
                        .argument("directory"));

    optionSet.addOption(Option("systemplate", "", "Path to a template tree with shared libraries etc to be used as source for chroot jails for child processes.")
                        .required(false)
                        .repeatable(false)
                        .argument("directory"));

    optionSet.addOption(Option("lotemplate", "", "Path to a LibreOffice installation tree to be copied (linked) into the jails for child processes. Should be on the same file system as systemplate.")
                        .required(false)
                        .repeatable(false)
                        .argument("directory"));

    optionSet.addOption(Option("childroot", "", "Path to the directory under which the chroot jails for the child processes will be created. Should be on the same file system as systemplate and lotemplate.")
                        .required(false)
                        .repeatable(false)
                        .argument("directory"));

    optionSet.addOption(Option("losubpath", "", "Relative path where the LibreOffice installation will be copied inside a jail (default: '" + loSubPath + "').")
                        .required(false)
                        .repeatable(false)
                        .argument("relative path"));

    optionSet.addOption(Option("numprespawns", "", "Number of child processes to keep started in advance and waiting for new clients.")
                        .required(false)
                        .repeatable(false)
                        .argument("number"));

    optionSet.addOption(Option("test", "", "Interactive testing.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("child", "", "For internal use only.")
                        .required(false)
                        .repeatable(false)
                        .argument("child id"));

    optionSet.addOption(Option("jail", "", "For internal use only.")
                        .required(false)
                        .repeatable(false)
                        .argument("directory"));

#if ENABLE_DEBUG
    optionSet.addOption(Option("uid", "", "Uid to assume if running under sudo for debugging purposes.")
                        .required(false)
                        .repeatable(false)
                        .argument("uid"));
#endif
}

void LOOLWSD::handleOption(const std::string& optionName, const std::string& value)
{
    ServerApplication::handleOption(optionName, value);

    if (optionName == "help")
    {
        displayHelp();
        exit(Application::EXIT_OK);
    }
    else if (optionName == "port")
        portNumber = std::stoi(value);
    else if (optionName == "cache")
        cache = value;
    else if (optionName == "systemplate")
        sysTemplate = value;
    else if (optionName == "lotemplate")
        loTemplate = value;
    else if (optionName == "childroot")
        childRoot = value;
    else if (optionName == "losubpath")
        loSubPath = value;
    else if (optionName == "numprespawns")
        _numPreSpawnedChildren = std::stoi(value);
    else if (optionName == "test")
        LOOLWSD::doTest = true;
    else if (optionName == "child")
        _childId = std::stoull(value);
    else if (optionName == "jail")
        jail = value;
#if ENABLE_DEBUG
    else if (optionName == "uid")
        uid = std::stoull(value);
#endif
}

void LOOLWSD::displayHelp()
{
    HelpFormatter helpFormatter(options());
    helpFormatter.setCommand(commandName());
    helpFormatter.setUsage("OPTIONS");
    helpFormatter.setHeader("LibreOffice On-Line WebSocket server.");
    helpFormatter.format(std::cout);
}

int LOOLWSD::createBroker()
{
    Process::Args args;

    args.push_back("--losubpath=" + LOOLWSD::loSubPath);
    args.push_back("--systemplate=" + sysTemplate);
    args.push_back("--lotemplate=" + loTemplate);
    args.push_back("--childroot=" + childRoot);
    args.push_back("--numprespawns=" + std::to_string(_numPreSpawnedChildren));

    std::string executable = Path(Application::instance().commandPath()).parent().toString() + "loolbroker";

    Log::info("Launching Broker: " + executable + " " +
              Poco::cat(std::string(" "), args.begin(), args.end()));

    ProcessHandle child = Process::launch(executable, args);

    MasterProcessSession::_childProcesses[child.id()] = child.id();

    return Application::EXIT_OK;
}

void LOOLWSD::startupBroker(const signed nBrokers)
{
    for (signed nCntr = nBrokers; nCntr > 0; --nCntr)
    {
        if (createBroker() < 0)
            break;
    }
}

int LOOLWSD::main(const std::vector<std::string>& /*args*/)
{
    Poco::Environment::set("LD_BIND_NOW", "1");
    Poco::Environment::set("LOK_VIEW_CALLBACK", "1");

    int status;
#ifdef __linux
    char *locale = setlocale(LC_ALL, NULL);
    if (locale == NULL || std::strcmp(locale, "C") == 0)
        setlocale(LC_ALL, "en_US.utf8");

    setSignals(false);
#endif

    Log::initialize("wsd");

    if (access(cache.c_str(), R_OK | W_OK | X_OK) != 0)
    {
        std::cout << Util::logPrefix() << "Unable to access " << cache <<
            ", please make sure it exists, and has write permission for this user." << std::endl;
        return Application::EXIT_UNAVAILABLE;
    }

    // We use the same option set for both parent and child loolwsd,
    // so must check options required in the parent (but not in the
    // child) separately now. Also check for options that are
    // meaningless for the parent.
    if (sysTemplate == "")
        throw MissingOptionException("systemplate");
    if (loTemplate == "")
        throw MissingOptionException("lotemplate");
    if (childRoot == "")
        throw MissingOptionException("childroot");

    if (_childId != 0)
        throw IncompatibleOptionsException("child");
    if (jail != "")
        throw IncompatibleOptionsException("jail");
    if (portNumber == MASTER_PORT_NUMBER)
        throw IncompatibleOptionsException("port");

    if (LOOLWSD::doTest)
        _numPreSpawnedChildren = 1;

    // log pid information
    {
        Poco::FileOutputStream filePID(LOOLWSD::PIDLOG);
        if (filePID.good())
            filePID << Process::id();
    }

    if (!File(FIFO_FILE).exists() && mkfifo(FIFO_FILE.c_str(), 0666) == -1)
    {
        std::cout << Util::logPrefix() << "Fail to create pipe FIFO" << std::endl;
        return Application::EXIT_UNAVAILABLE;
    }

    _namedMutexLOOL.lock();

    startupBroker(1);

#ifdef __linux
    dropCapability(CAP_SYS_CHROOT);
    dropCapability(CAP_MKNOD);
    dropCapability(CAP_FOWNER);
#else
    dropCapability();
#endif

    // Start a server listening on the port for clients
    ServerSocket svs(portNumber, _numPreSpawnedChildren*10);
    ThreadPool threadPool(_numPreSpawnedChildren*2, _numPreSpawnedChildren*5);
    HTTPServer srv(new RequestHandlerFactory(), threadPool, svs, new HTTPServerParams);

    srv.start();

    // And one on the port for child processes
    SocketAddress addr2("127.0.0.1", MASTER_PORT_NUMBER);
    ServerSocket svs2(addr2, _numPreSpawnedChildren);
    ThreadPool threadPool2(_numPreSpawnedChildren*2, _numPreSpawnedChildren*5);
    HTTPServer srv2(new RequestHandlerFactory(), threadPool2, svs2, new HTTPServerParams);

    srv2.start();

    if ( (writerBroker = open(FIFO_FILE.c_str(), O_WRONLY) ) < 0 )
    {
        Log::error("Error: failed to open pipe [" + FIFO_FILE + "] write only.");
        return Application::EXIT_UNAVAILABLE;
    }

    _namedMutexLOOL.unlock();

    TestInput input(*this, svs, srv);
    Thread inputThread;
    if (LOOLWSD::doTest)
    {
        inputThread.start(input);
        waitForTerminationRequest();
    }

    while (!LOOLWSD::isShutDown && !LOOLWSD::doTest && MasterProcessSession::_childProcesses.size() > 0)
    {
        pid_t pid = waitpid(-1, &status, WUNTRACED | WNOHANG);
        if (pid > 0)
        {
            if ( MasterProcessSession::_childProcesses.find(pid) != MasterProcessSession::_childProcesses.end() )
            {
                if ((WIFEXITED(status) || WIFSIGNALED(status) || WTERMSIG(status) ) )
                {
                    Log::error("Child [" + std::to_string(pid) + "] processes died.");
                    MasterProcessSession::_childProcesses.erase(pid);
                }

                if ( WCOREDUMP(status) )
                    Log::error("Child [" + std::to_string(pid) + "] produced a core dump.");

                if ( WIFSTOPPED(status) )
                    Log::error("Child [" + std::to_string(pid) + "] process was stopped by delivery of a signal.");

                if ( WSTOPSIG(status) )
                    Log::error("Child [" + std::to_string(pid) + "] process was stopped.");

                if ( WIFCONTINUED(status) )
                    Log::error("Child [" + std::to_string(pid) + "] process was resumed.");
            }
            else
            {
                Log::error("None of our known child processes died. PID: " + std::to_string(pid));
            }
        }
        else if (pid < 0)
            Log::error("Error: Child error.");

        ++timeoutCounter;
        if (timeoutCounter == INTERVAL_PROBES)
        {
            timeoutCounter = 0;
            sleep(MAINTENANCE_INTERVAL*2);
        }
    }

    if (LOOLWSD::doTest)
        inputThread.join();

    // stop the service, no more request
    srv.stop();
    srv2.stop();

    // close all websockets
    threadPool.joinAll();
    threadPool2.joinAll();

    // Terminate child processes
    for (auto i : MasterProcessSession::_childProcesses)
    {
        Log::info("Requesting child process " + std::to_string(i.first) + " to terminate");
        Process::requestTermination(i.first);
    }

    // wait broker process finish
    waitpid(-1, &status, WUNTRACED);

    Log::info("loolwsd finished OK!");
    return Application::EXIT_OK;
}

POCO_SERVER_MAIN(LOOLWSD)

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
