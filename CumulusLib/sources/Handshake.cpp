/* 
	Copyright 2010 OpenRTMFP
 
	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License received along this program for more
	details (or else see http://www.gnu.org/licenses/).

	This file is a part of Cumulus.
*/

#include "Handshake.h"
#include "Logs.h"
#include "Util.h"
#include "Poco/RandomStream.h"
#include <openssl/evp.h>
#include "string.h"

using namespace std;
using namespace Poco;
using namespace Poco::Net;

namespace Cumulus {

Handshake::Handshake(ReceivingEngine& receivingEngine,SendingEngine& sendingEngine,Gateway& gateway,DatagramSocket& edgesSocket,Handler& handler,Entity& entity) : ServerSession(receivingEngine,sendingEngine,0,0,Peer(handler),RTMFP_SYMETRIC_KEY,RTMFP_SYMETRIC_KEY,(Invoker&)handler),
	_gateway(gateway),_edgesSocket(edgesSocket) {
	(bool&)checked=true;

	memcpy(_certificat,"\x01\x0A\x41\x0E",4);
	RandomInputStream().read((char*)&_certificat[4],64);
	memcpy(&_certificat[68],"\x02\x15\x02\x02\x15\x05\x02\x15\x0E",9);

	// Display far id flash side
	EVP_Digest(_certificat,sizeof(_certificat),(unsigned char *)entity.id,NULL,EVP_sha256(),NULL);
}


Handshake::~Handshake() {
	fail(""); // To avoid the failSignal
}

void Handshake::manage() {
	// delete obsolete cookie
	map<const UInt8*,Cookie*,CompareCookies>::iterator it=_cookies.begin();
	while(it!=_cookies.end()) {
		if(it->second->obsolete()) {
			if(!it->second->light)
				eraseHelloAttempt(it->second->tag);
			DEBUG("Obsolete cookie : %s",Util::FormatHex(it->first,COOKIE_SIZE).c_str());
			delete it->second;
			_cookies.erase(it++);
		} else
			++it;
	}

	// keepalive edges
	map<string,Edge*>::iterator it2=edges().begin();
	while(it2!=edges().end()) {
		if(it2->second->obsolete()) {
			if(it2->second->raise()) {
				NOTE("RTMFP server edge %s lost",it2->first.c_str());
				UInt32 newBufferSize = edges().size()*_invoker.udpBufferSize;
				_edgesSocket.setReceiveBufferSize(newBufferSize);_edgesSocket.setReceiveBufferSize(newBufferSize);
				delete it2->second;
				edges().erase(it2++);
				continue;
			};
			PacketWriter& packet(writer());
			packet.write8(0x40);
			packet.write16(0);
			setEndPoint(_edgesSocket,SocketAddress(it2->first));
			nextDumpAreMiddle=true;
			flush(AESEngine::EMPTY);
			INFO("Keepalive RTMFP server edge %s",it2->first.c_str());
		}
		++it2;
	}
}

void Handshake::commitCookie(const Session& session) {
	(bool&)session.checked = true;
	map<const UInt8*,Cookie*,CompareCookies>::iterator it;
	for(it=_cookies.begin();it!=_cookies.end();++it) {
		if(it->second->id==session.id) {
			if(!it->second->light)
				eraseHelloAttempt(it->second->tag);
			delete it->second;
			_cookies.erase(it);
			return;
		}
	}
	WARN("Cookie of the session %u not found",session.id);
}

void Handshake::clear() {
	// delete cookies
	map<const UInt8*,Cookie*,CompareCookies>::const_iterator it;
	for(it=_cookies.begin();it!=_cookies.end();++it) {
		if(!it->second->light)
			eraseHelloAttempt(it->second->tag);
		delete it->second;
	}
	_cookies.clear();
	// clear edges and warn them on server death
	map<string,Edge*>::iterator it2;
	for(it2=edges().begin();it2!=edges().end();++it2) {
		PacketWriter& packet(writer());
		packet.write8(0x45);
		packet.write16(0);
		setEndPoint(_edgesSocket,SocketAddress(it2->first));
		nextDumpAreMiddle=true;
		flush(AESEngine::EMPTY);
		delete it2->second;
	}
	edges().clear();
	_edgesSocket.setReceiveBufferSize(_invoker.udpBufferSize);_edgesSocket.setReceiveBufferSize(_invoker.udpBufferSize);
}

void Handshake::createCookie(PacketWriter& writer,HelloAttempt& attempt,const string& tag,const string& queryUrl) {
	// New Cookie
	Cookie* pCookie = attempt.pCookie;
	if(!pCookie) {
		if(attempt.pTarget)
			pCookie = new Cookie(tag,*attempt.pTarget);
		else
			pCookie = new Cookie(tag,queryUrl);
		_cookies[pCookie->value] =  pCookie;
		attempt.pCookie = pCookie;
	}
	writer.write8(COOKIE_SIZE);
	writer.writeRaw(pCookie->value,COOKIE_SIZE);
}


bool Handshake::updateEdge(PacketReader& request) {
	string address = peer.address.toString();
	map<string,Edge*>::iterator it = edges().lower_bound(address);
	if(it!=edges().end() && it->first==address) {
		it->second->update();
		return false;
	}
	if(it!=edges().begin())
		--it;

	NOTE("New RTMFP server edge %s",address.c_str());

	Edge* pEdgeDescriptor = new Edge();
	request.readAddress(pEdgeDescriptor->address);
	edges().insert(it,pair<string,Edge*>(address,pEdgeDescriptor));
	UInt32 newBufferSize = (edges().size()+1)*_invoker.udpBufferSize;
	_edgesSocket.setReceiveBufferSize(newBufferSize);_edgesSocket.setReceiveBufferSize(newBufferSize);
	return true;
}

void Handshake::packetHandler(PacketReader& packet) {

	UInt8 marker = packet.read8();
	if(marker!=0x0b) {
		ERROR("Marker handshake wrong : should be 0b and not %u",marker);
		return;
	}
	
	UInt16 time = packet.read16();
	UInt8 id = packet.read8();
	packet.shrink(packet.read16()); // length

	PacketWriter& response(writer());
	UInt32 pos = response.position();
	response.next(3);
	UInt8 idResponse = handshakeHandler(id,packet,response);
	response.reset(pos);
	if(idResponse>0) {
		response.write8(idResponse);
		response.write16(response.length()-response.position()-2);
		flush();
	}

	// reset farid to 0!
	(UInt32&)farId=0;
}


UInt8 Handshake::handshakeHandler(UInt8 id,PacketReader& request,PacketWriter& response) {

	switch(id){
		case 0x30: {
			
			request.read8(); // passer un caractere (boite dans boite)
			UInt8 epdLen = request.read8()-1;

			UInt8 type = request.read8();

			string epd;
			request.readRaw(epdLen,epd);

			string tag;
			request.readRaw(16,tag);
			response.writeString8(tag);
			
			if(type == 0x0f)
				return _gateway.p2pHandshake(tag,response,peer.address,(const UInt8*)epd.c_str());

			if(type == 0x0a){
				/// Handshake
				HelloAttempt& attempt = helloAttempt<HelloAttempt>(tag);
				if(edges().size()>0 && (_invoker.edgesAttemptsBeforeFallback==0 || attempt.count <_invoker.edgesAttemptsBeforeFallback)) {
					
					if(_invoker.edgesAttemptsBeforeFallback>0) {
						try {
							URI uri(epd);
							response.writeAddress(SocketAddress(uri.getHost(),uri.getPort()),false); // TODO check with true!
						} catch(Exception& ex) {
							ERROR("Parsing %s URL problem in hello attempt : %s",epd.c_str(),ex.displayText().c_str());
						}
					}

					map<int,list<Edge*> > edgesSortedByCount;
					map<string,Edge*>::const_iterator it;
					for(it=edges().begin();it!=edges().end();++it)
						edgesSortedByCount[it->second->count].push_back(it->second);

					UInt8 count=0;
					map<int,list<Edge*> >::const_iterator it2;
					for(it2=edgesSortedByCount.begin();it2!=edgesSortedByCount.end();++it2) {
						list<Edge*>::const_iterator it3;
						for(it3=it2->second.begin();it3!=it2->second.end();++it3) {
							response.writeAddress((*it3)->address,false);
							if((++count)==6) // 6 redirections maximum
								break;
						}
						if(it3!=it2->second.end())
							break;
					}
					return 0x71;

				}

				if(edges().size()>0)
					WARN("After %u hello attempts, impossible to connect to edges. Edges are busy? or unreachable?",_invoker.edgesAttemptsBeforeFallback);
	
				// New Cookie
				createCookie(response,attempt,tag,epd);
				 
				// instance id (certificat in the middle)
				response.writeRaw(_certificat,sizeof(_certificat));
				
				return 0x70;
			} else
				ERROR("Unkown handshake first way with '%02x' type",type);
			break;
		}
		case 0x39:
		case 0x38: {
			(UInt32&)farId = request.read32();

			if(request.read7BitLongValue()!=COOKIE_SIZE) {
				ERROR("Bad handshake cookie '%s': its size should be 64 bytes",Util::FormatHex(request.current(),COOKIE_SIZE).c_str());
				return 0;
			}
	
			map<const UInt8*,Cookie*,CompareCookies>::iterator itCookie = _cookies.find(request.current());
			if(itCookie==_cookies.end()) {
				if(id!=0x39) {
					ERROR("Handshake cookie '%s' unknown",Util::FormatHex(request.current(),COOKIE_SIZE).c_str());
					return 0;
				}
				Cookie* pCookie = new Cookie();
				UInt32 pos = request.position();
				request.readRaw((UInt8*)pCookie->value,COOKIE_SIZE);
				request >> (string&)pCookie->queryUrl;
				request.reset(pos);
				itCookie = _cookies.insert(pair<const UInt8*,Cookie*>(pCookie->value,pCookie)).first;
			}

			Cookie& cookie(*itCookie->second);

			if(cookie.id==0) {

				UInt8 decryptKey[AES_KEY_SIZE];UInt8* pDecryptKey=&decryptKey[0];
				UInt8 encryptKey[AES_KEY_SIZE];UInt8* pEncryptKey=&encryptKey[0];

				if(id==0x38) {
					request.next(COOKIE_SIZE);
					size_t size = (size_t)request.read7BitLongValue();
					// peerId = SHA256(farPubKey)
					EVP_Digest(request.current(),size,(UInt8*)peer.id,NULL,EVP_sha256(),NULL);

					vector<UInt8> publicKey(request.read7BitValue()-2);
					request.next(2); // unknown
					request.readRaw(&publicKey[0],publicKey.size());

					size = request.read7BitValue();

					cookie.computeKeys(&publicKey[0],publicKey.size(),request.current(),(UInt16)size,decryptKey,encryptKey);
				} else {
					// edge
					pDecryptKey=NULL;
					pEncryptKey=NULL;
					memcpy((UInt8*)peer.id,request.current(),ID_SIZE);
					request.next(COOKIE_SIZE);
					request.next(request.read7BitEncoded());
				}

				// Fill peer infos
				Util::UnpackUrl(cookie.queryUrl,(string&)peer.path,(map<string,string>&)peer.properties);

				// RESPONSE
				Session& session = _gateway.createSession(farId,peer,pDecryptKey,pEncryptKey,cookie);
				(UInt32&)cookie.id = session.id;

				string address;
				if(id==0x39) {
					// Session by edge 
					session.flags |= SESSION_BY_EDGE;
					Edge* pEdge = _invoker.edges(peer.address);
					if(!pEdge)
						ERROR("Edge session creation by an unknown server edge %s",peer.address.toString().c_str())
					else
						pEdge->addSession(session);
					request >> address;
				} else // Session direct
					address = session.peer.address.toString();

				session.peer.addresses.clear();
				session.peer.addresses.push_back(address);

				cookie.write();
			} else
				_gateway.repeatCookie(farId,cookie);
			if(cookie.response)
				cookie.read(response);

			return cookie.response;
		}
		case 0x41: {
			nextDumpAreMiddle=true;
			// EDGE HELLO Message
			if(updateEdge(request))
				return 0x40; // If new edge, we answer with the keepalive message
			break;
		}
		case 0x45: {
			nextDumpAreMiddle=true;
			string address = peer.address.toString();
			// EDGE DEATH Message
			map<string,Edge*>::iterator it = edges().find(address);
			if(it==edges().end()) {
				WARN("Death message for an unknown edge");
				break;
			}
			NOTE("RTMFP server edge %s death",address.c_str());
			UInt32 newBufferSize = edges().size()*_invoker.udpBufferSize;
			_edgesSocket.setReceiveBufferSize(newBufferSize);_edgesSocket.setReceiveBufferSize(newBufferSize);
			delete it->second;
			edges().erase(it);
			break;
		}
		default:
			ERROR("Unkown handshake packet id %u",id);
	}

	return 0;
}




} // namespace Cumulus
