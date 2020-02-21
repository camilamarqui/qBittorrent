/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2019  Mike Tzou (Chocobo1)
 * Copyright (C) 2015  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2006  Christophe Dumez <chris@qbittorrent.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "tracker.h"

#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>

#include <QHostAddress>

#include "base/exceptions.h"
#include "base/global.h"
#include "base/http/httperror.h"
#include "base/http/server.h"
#include "base/http/types.h"
#include "base/logger.h"
#include "base/preferences.h"

namespace
{
    // static limits
    const int MAX_TORRENTS = 10000;
    const int MAX_PEERS_PER_TORRENT = 200;
    const int ANNOUNCE_INTERVAL = 1800;  // 30min

    // constants
    const int PEER_ID_SIZE = 20;

    const char ANNOUNCE_REQUEST_PATH[] = "/announce";

    const char ANNOUNCE_REQUEST_COMPACT[] = "compact";
    const char ANNOUNCE_REQUEST_INFO_HASH[] = "info_hash";
    const char ANNOUNCE_REQUEST_IP[] = "ip";
    const char ANNOUNCE_REQUEST_LEFT[] = "left";
    const char ANNOUNCE_REQUEST_NO_PEER_ID[] = "no_peer_id";
    const char ANNOUNCE_REQUEST_NUM_WANT[] = "numwant";
    const char ANNOUNCE_REQUEST_PEER_ID[] = "peer_id";
    const char ANNOUNCE_REQUEST_PORT[] = "port";

    const char ANNOUNCE_REQUEST_EVENT[] = "event";
    const char ANNOUNCE_REQUEST_EVENT_COMPLETED[] = "completed";
    const char ANNOUNCE_REQUEST_EVENT_EMPTY[] = "empty";
    const char ANNOUNCE_REQUEST_EVENT_STARTED[] = "started";
    const char ANNOUNCE_REQUEST_EVENT_STOPPED[] = "stopped";
    const char ANNOUNCE_REQUEST_EVENT_PAUSED[] = "paused";

    const char ANNOUNCE_RESPONSE_COMPLETE[] = "complete";
    const char ANNOUNCE_RESPONSE_EXTERNAL_IP[] = "external ip";
    const char ANNOUNCE_RESPONSE_FAILURE_REASON[] = "failure reason";
    const char ANNOUNCE_RESPONSE_INCOMPLETE[] = "incomplete";
    const char ANNOUNCE_RESPONSE_INTERVAL[] = "interval";
    const char ANNOUNCE_RESPONSE_PEERS6[] = "peers6";
    const char ANNOUNCE_RESPONSE_PEERS[] = "peers";

    const char ANNOUNCE_RESPONSE_PEERS_IP[] = "ip";
    const char ANNOUNCE_RESPONSE_PEERS_PEER_ID[] = "peer id";
    const char ANNOUNCE_RESPONSE_PEERS_PORT[] = "port";

    class TrackerError : public RuntimeError
    {
    public:
        using RuntimeError::RuntimeError;
    };

    QByteArray toBigEndianByteArray(const QHostAddress &addr)
    {
        // translate IP address to a sequence of bytes in big-endian order
        switch (addr.protocol()) {
        case QAbstractSocket::IPv4Protocol:
        case QAbstractSocket::AnyIPProtocol: {
                const quint32 ipv4 = addr.toIPv4Address();
                QByteArray ret;
                ret.append(static_cast<char>((ipv4 >> 24) & 0xFF))
                   .append(static_cast<char>((ipv4 >> 16) & 0xFF))
                   .append(static_cast<char>((ipv4 >> 8) & 0xFF))
                   .append(static_cast<char>(ipv4 & 0xFF));
                return ret;
            }

        case QAbstractSocket::IPv6Protocol: {
                const Q_IPV6ADDR ipv6 = addr.toIPv6Address();
                QByteArray ret;
                for (const quint8 i : ipv6.c)
                    ret.append(i);
                return ret;
            }

        case QAbstractSocket::UnknownNetworkLayerProtocol:
        default:
            return {};
        };
    }
}

namespace BitTorrent
{
    // Peer
    QByteArray Peer::uniqueID() const
    {
        return (QByteArray::fromStdString(address) + ':' + QByteArray::number(port));
    }

    bool operator==(const Peer &left, const Peer &right)
    {
        return (left.uniqueID() == right.uniqueID());
    }

    bool operator!=(const Peer &left, const Peer &right)
    {
        return !(left == right);
    }

    uint qHash(const Peer &key, const uint seed)
    {
        return qHash(key.uniqueID(), seed);
    }
}

using namespace BitTorrent;

// TrackerAnnounceRequest
struct Tracker::TrackerAnnounceRequest
{
    QHostAddress socketAddress;
    QByteArray claimedAddress;  // self claimed by peer
    InfoHash infoHash;
    QString event;
    Peer peer;
    int numwant = 50;
    bool compact = true;
    bool noPeerId = false;
};

// Tracker::TorrentStats
void Tracker::TorrentStats::setPeer(const Peer &peer)
{
    // always replace existing peer
    if (!removePeer(peer)) {
        // Too many peers, remove a random one
        if (peers.size() >= MAX_PEERS_PER_TORRENT)
            removePeer(*peers.begin());
    }

    // add peer
    if (peer.isSeeder)
        ++seeders;
    peers.insert(peer);
}

bool Tracker::TorrentStats::removePeer(const Peer &peer)
{
    const auto iter = peers.find(peer);
    if (iter == peers.end())
        return false;

    if (iter->isSeeder)
        --seeders;
    peers.remove(*iter);
    return true;
}

// Tracker
Tracker::Tracker(QObject *parent)
    : QObject(parent)
    , m_server(new Http::Server(this, this))
{
}

bool Tracker::start()
{
    const QHostAddress ip = QHostAddress::Any;
    const int port = Preferences::instance()->getTrackerPort();

    if (m_server->isListening()) {
        if (m_server->serverPort() == port) {
            // Already listening on the right port, just return
            return true;
        }

        // Wrong port, closing the server
        m_server->close();
    }

    // Listen on the predefined port
    const bool listenSuccess = m_server->listen(ip, port);

    if (listenSuccess) {
        LogMsg(tr("Embedded Tracker: Now listening on IP: %1, port: %2")
            .arg(ip.toString(), QString::number(port)), Log::INFO);
    }
    else {
        LogMsg(tr("Embedded Tracker: Unable to bind to IP: %1, port: %2. Reason: %3")
                .arg(ip.toString(), QString::number(port), m_server->errorString())
            , Log::WARNING);
    }

    return listenSuccess;
}

Http::Response Tracker::processRequest(const Http::Request &request, const Http::Environment &env)
{
    clear();  // clear response

    m_request = request;
    m_env = env;

    status(200);

    try {
        // Is it a GET request?
        if (request.method != Http::HEADER_REQUEST_METHOD_GET)
            throw MethodNotAllowedHTTPError();

        if (request.path.toLower().startsWith(ANNOUNCE_REQUEST_PATH))
            processAnnounceRequest();
        else
            throw NotFoundHTTPError();
    }
    catch (const HTTPError &error) {
        status(error.statusCode(), error.statusText());
        if (!error.message().isEmpty())
            print(error.message(), Http::CONTENT_TYPE_TXT);
    }
    catch (const TrackerError &error) {
        clear();  // clear response
        status(200);

        const lt::entry::dictionary_type bencodedEntry = {
            {ANNOUNCE_RESPONSE_FAILURE_REASON, {error.what()}}
        };
        QByteArray reply;
        lt::bencode(std::back_inserter(reply), bencodedEntry);
        print(reply, Http::CONTENT_TYPE_TXT);
    }

    return response();
}

void Tracker::processAnnounceRequest()
{
    const QHash<QString, QByteArray> &queryParams = m_request.query;
    TrackerAnnounceRequest announceReq;

    // ip address
    announceReq.socketAddress = m_env.clientAddress;
    announceReq.claimedAddress = queryParams.value(ANNOUNCE_REQUEST_IP);

    // 1. info_hash
    const auto infoHashIter = queryParams.find(ANNOUNCE_REQUEST_INFO_HASH);
    if (infoHashIter == queryParams.end())
        throw TrackerError("Missing \"info_hash\" parameter");

    const InfoHash infoHash(infoHashIter->toHex());
    if (!infoHash.isValid())
        throw TrackerError("Invalid \"info_hash\" parameter");

    announceReq.infoHash = infoHash;

    // 2. peer_id
    const auto peerIdIter = queryParams.find(ANNOUNCE_REQUEST_PEER_ID);
    if (peerIdIter == queryParams.end())
        throw TrackerError("Missing \"peer_id\" parameter");

    if (peerIdIter->size() > PEER_ID_SIZE)
        throw TrackerError("Invalid \"peer_id\" parameter");

    announceReq.peer.peerId = *peerIdIter;

    // 3. port
    const auto portIter = queryParams.find(ANNOUNCE_REQUEST_PORT);
    if (portIter == queryParams.end())
        throw TrackerError("Missing \"port\" parameter");

    const ushort portNum = portIter->toUShort();
    if (portNum == 0)
        throw TrackerError("Invalid \"port\" parameter");

    announceReq.peer.port = portNum;

    // 4. numwant
    const auto numWantIter = queryParams.find(ANNOUNCE_REQUEST_NUM_WANT);
    if (numWantIter != queryParams.end()) {
        const int num = numWantIter->toInt();
        if (num < 0)
            throw TrackerError("Invalid \"numwant\" parameter");
        announceReq.numwant = num;
    }

    // 5. no_peer_id
    // non-formal extension
    announceReq.noPeerId = (queryParams.value(ANNOUNCE_REQUEST_NO_PEER_ID) == "1");

    // 6. left
    announceReq.peer.isSeeder = (queryParams.value(ANNOUNCE_REQUEST_LEFT) == "0");

    // 7. compact
    announceReq.compact = (queryParams.value(ANNOUNCE_REQUEST_COMPACT) != "0");

    // 8. cache `peers` field so we don't recompute when sending response
    const QHostAddress claimedIPAddress {QString::fromLatin1(announceReq.claimedAddress)};
    announceReq.peer.endpoint = toBigEndianByteArray(!claimedIPAddress.isNull() ? claimedIPAddress : announceReq.socketAddress)
        .append(static_cast<char>((announceReq.peer.port >> 8) & 0xFF))
        .append(static_cast<char>(announceReq.peer.port & 0xFF))
        .toStdString();

    // 9. cache `address` field so we don't recompute when sending response
    announceReq.peer.address = !announceReq.claimedAddress.isEmpty()
        ? announceReq.claimedAddress.constData()
        : announceReq.socketAddress.toString().toLatin1().constData(),

    // 10. event
    announceReq.event = queryParams.value(ANNOUNCE_REQUEST_EVENT);

    if (announceReq.event.isEmpty()
        || (announceReq.event == ANNOUNCE_REQUEST_EVENT_EMPTY)
        || (announceReq.event == ANNOUNCE_REQUEST_EVENT_COMPLETED)
        || (announceReq.event == ANNOUNCE_REQUEST_EVENT_STARTED)
        || (announceReq.event == ANNOUNCE_REQUEST_EVENT_PAUSED)) {
        // [BEP-21] Extension for partial seeds (partial support)
        registerPeer(announceReq);
        prepareAnnounceResponse(announceReq);
    }
    else if (announceReq.event == ANNOUNCE_REQUEST_EVENT_STOPPED) {
        unregisterPeer(announceReq);
    }
    else {
        throw TrackerError("Invalid \"event\" parameter");
    }
}

void Tracker::registerPeer(const TrackerAnnounceRequest &announceReq)
{
    if (!m_torrents.contains(announceReq.infoHash)) {
        // Reached max size, remove a random torrent
        if (m_torrents.size() >= MAX_TORRENTS)
            m_torrents.erase(m_torrents.begin());
    }

    m_torrents[announceReq.infoHash].setPeer(announceReq.peer);
}

void Tracker::unregisterPeer(const TrackerAnnounceRequest &announceReq)
{
    const auto torrentStatsIter = m_torrents.find(announceReq.infoHash);
    if (torrentStatsIter == m_torrents.end())
        return;

    torrentStatsIter->removePeer(announceReq.peer);

    if (torrentStatsIter->peers.isEmpty())
        m_torrents.erase(torrentStatsIter);
}

void Tracker::prepareAnnounceResponse(const TrackerAnnounceRequest &announceReq)
{
    const TorrentStats &torrentStats = m_torrents[announceReq.infoHash];

    lt::entry::dictionary_type replyDict {
        {ANNOUNCE_RESPONSE_INTERVAL, ANNOUNCE_INTERVAL},
        {ANNOUNCE_RESPONSE_COMPLETE, torrentStats.seeders},
        {ANNOUNCE_RESPONSE_INCOMPLETE, (torrentStats.peers.size() - torrentStats.seeders)},

        // [BEP-24] Tracker Returns External IP
        {ANNOUNCE_RESPONSE_EXTERNAL_IP, toBigEndianByteArray(announceReq.socketAddress).toStdString()}
    };

    // peer list
    // [BEP-7] IPv6 Tracker Extension (partial support)
    // [BEP-23] Tracker Returns Compact Peer Lists
    if (announceReq.compact) {
        lt::entry::list_type peerList;
        lt::entry::list_type peer6List;

        int counter = 0;
        for (const Peer &peer : asConst(torrentStats.peers)) {
            if (counter++ >= announceReq.numwant)
                break;

            if (peer.endpoint.size() == 6)  // IPv4
                peerList.emplace_back(peer.endpoint);
            else if (peer.endpoint.size() == 18)  // IPv6
                peer6List.emplace_back(peer.endpoint);
        }

        replyDict[ANNOUNCE_RESPONSE_PEERS] = peerList;  // required, even it's empty
        if (!peer6List.empty())
            replyDict[ANNOUNCE_RESPONSE_PEERS6] = peer6List;
    }
    else {
        lt::entry::list_type peerList;

        int counter = 0;
        for (const Peer &peer : torrentStats.peers) {
            if (counter++ >= announceReq.numwant)
                break;

            lt::entry::dictionary_type peerDict = {
                {ANNOUNCE_RESPONSE_PEERS_IP, peer.address},
                {ANNOUNCE_RESPONSE_PEERS_PORT, peer.port}
            };

            if (!announceReq.noPeerId)
                peerDict[ANNOUNCE_RESPONSE_PEERS_PEER_ID] = peer.peerId.constData();

            peerList.emplace_back(peerDict);
        }

        replyDict[ANNOUNCE_RESPONSE_PEERS] = peerList;
    }

    // bencode
    QByteArray reply;
    lt::bencode(std::back_inserter(reply), replyDict);
    print(reply, Http::CONTENT_TYPE_TXT);
}
