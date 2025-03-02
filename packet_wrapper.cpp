// packetgeneratorCpp.cpp : Defines the entry point for the console application.
//

//#include "stdafx.h" //for visual studio
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <iomanip>
#include <cstdint>
#include <cstring>

//----------------------------------------------------
// Fixed-size header structure.
// Source and destination are fixed to 10 characters.
struct PacketHeader {
	uint8_t msgType;        // Message type
	uint8_t sequence;       // Sequence number
	char source[10];        // Source identifier (null-padded)
	char dest[10];          // Destination identifier (null-padded)
	// Checksum (BCC) is computed over the full frame and appended later.
};

//----------------------------------------------------
// PacketBuffer: low-level pointer wrapper for a contiguous byte buffer.
// Its methods return an advanced PacketBuffer (with updated offset)
// so you can chain calls.
class PacketBuffer {
public:
	static PacketBuffer createReadOnly(const unsigned char* roBuffer,
		unsigned int offset, unsigned int size) {
		return PacketBuffer(roBuffer, nullptr, offset, size);
	}
	static PacketBuffer createReadWrite(unsigned char* rwBuffer,
		unsigned int offset, unsigned int size) {
		return PacketBuffer(nullptr, rwBuffer, offset, size);
	}
	static PacketBuffer nullBuffer() {
		return PacketBuffer(nullptr, nullptr, 0, 0);
	}

	unsigned int offset() const { return m_offset; }
	unsigned int size() const { return m_size; }

	// Write an 8-bit unsigned value.
	PacketBuffer writeUInt8(uint8_t value) {
		if (m_rwBuffer && m_offset < m_size) {
			m_rwBuffer[m_offset] = value;
		}
		return advance(1);
	}

	// Read an 8-bit unsigned value.
	PacketBuffer readUInt8(uint8_t &value) const {
		if (m_roBuffer && m_offset < m_size) {
			value = m_roBuffer[m_offset];
		}
		return advance(1);
	}

	// Write a structure of type T into the buffer.
	template<typename T>
	PacketBuffer writeStruct(const T &data) {
		if (m_rwBuffer && m_offset + sizeof(T) <= m_size) {
			*reinterpret_cast<T*>(m_rwBuffer + m_offset) = data;
		}
		return advance(sizeof(T));
	}

	// Read a structure of type T from the buffer.
	template<typename T>
	PacketBuffer readStruct(T &data) const {
		if (m_roBuffer && m_offset + sizeof(T) <= m_size) {
			data = *reinterpret_cast<const T*>(m_roBuffer + m_offset);
		}
		return advance(sizeof(T));
	}

	friend class ContiguousPacket;

private:
	PacketBuffer(const unsigned char* ro, unsigned char* rw,
		unsigned int offset, unsigned int size)
		: m_roBuffer(ro), m_rwBuffer(rw), m_offset(offset), m_size(size) {}

	PacketBuffer advance(int count) const {
		return PacketBuffer(m_roBuffer, m_rwBuffer, m_offset + count, (m_size > count ? m_size - count : 0));
	}

	const unsigned char* m_roBuffer; // For read-only access
	unsigned char* m_rwBuffer;         // For write access
	unsigned int m_offset;
	unsigned int m_size;
};

//----------------------------------------------------
// Abstract base class for a network packet.
// Note: writePayload() returns a reference to NetworkPacket to enable covariant returns.
class NetworkPacket {
public:
	virtual ~NetworkPacket() {}
	// Write external payload data into the packet; returns a reference for chaining.
	virtual NetworkPacket& writePayload(PacketBuffer &buffer, int payloadLength) = 0;
	virtual bool readPayload(PacketBuffer &buffer) = 0;
	virtual uint8_t messageType() const = 0;

	// Header setters/getters.
	void setSource(const std::string &src) { m_source = src; }
	std::string source() const { return m_source; }
	void setDest(const std::string &dest) { m_dest = dest; }
	std::string dest() const { return m_dest; }
	void setSequenceNumber(uint8_t seq) { m_sequence = seq; }
	uint8_t sequenceNumber() const { return m_sequence; }
	void setChecksum(uint8_t cs) { m_checksum = cs; }
	uint8_t checksum() const { return m_checksum; }

protected:
	std::string m_source;
	std::string m_dest;
	uint8_t m_sequence = 0;
	uint8_t m_checksum = 0;
};
/*
template<typename Derived>
class NetworkPacketGenerator : public NetworkPacket {
public:
    	NetworkPacket* clone() const override {
        	return new Derived();
    	}	
	virtual std::string name() const {
		return typeid(Derived).name();
	}
};
*/
//----------------------------------------------------
// ContiguousPacket holds the entire frame in one continuous memory block.
// The frame format is: [header | payload | checksum].
// Chained header setters and payload writers modify m_frame.
class ContiguousPacket : public NetworkPacket { //public NetworkPacketGenerator<ContiguousPacket> {
public:
	ContiguousPacket() : m_msgType(0) {
		// Reserve space for the header.
		m_frame.resize(sizeof(PacketHeader), 0);
	}
	ContiguousPacket(uint8_t msgType) : m_msgType(msgType) {
		m_frame.resize(sizeof(PacketHeader), 0);
	}
	virtual ~ContiguousPacket() {}

	// Chained header setters.
	ContiguousPacket& setMessageType(uint8_t type) {
		PacketHeader* header = reinterpret_cast<PacketHeader*>(m_frame.data());
		header->msgType = type;
		m_msgType = type;
		return *this;
	}
	ContiguousPacket& setSequenceNumber(uint8_t seq) {
		PacketHeader* header = reinterpret_cast<PacketHeader*>(m_frame.data());
		header->sequence = seq;
		m_sequence = seq;
		return *this;
	}
	ContiguousPacket& setSource(const std::string& src) {
		PacketHeader* header = reinterpret_cast<PacketHeader*>(m_frame.data());
		std::fill(std::begin(header->source), std::end(header->source), '\0');
		std::copy_n(src.begin(), std::min(src.size(), size_t(10)), header->source);
		m_source = src;
		return *this;
	}
	ContiguousPacket& setDest(const std::string& dst) {
		PacketHeader* header = reinterpret_cast<PacketHeader*>(m_frame.data());
		std::fill(std::begin(header->dest), std::end(header->dest), '\0');
		std::copy_n(dst.begin(), std::min(dst.size(), size_t(10)), header->dest);
		m_dest = dst;
		return *this;
	}

	// Chained payload writer that copies external payload data into the internal frame.
	// This method appends payload bytes after the header.
	ContiguousPacket& writePayload(PacketBuffer &buffer, int payloadLength) override {
		if (buffer.size() < static_cast<unsigned int>(payloadLength) || !buffer.m_roBuffer)
			return *this;
		m_frame.insert(m_frame.end(),
			buffer.m_roBuffer + buffer.offset(),
			buffer.m_roBuffer + buffer.offset() + payloadLength);
		return *this;
	}

	// Additional chained write for a single byte.
	ContiguousPacket& writeUInt8(uint8_t value) {
		m_frame.push_back(static_cast<char>(value));
		return *this;
	}

	// Not implemented for this demo.
	bool readPayload(PacketBuffer &buffer) override {
		return false;
	}

	uint8_t messageType() const override { return m_msgType; }

	// Finalize the packet by computing the checksum (BCC) as the XOR of all header and payload bytes,
	// then appending it to the frame.
	ContiguousPacket& finalize() {
		uint8_t bcc = 0;
		for (uint8_t byte : m_frame) {
			bcc ^= byte;
		}
		m_checksum = bcc;
		m_frame.push_back(static_cast<char>(bcc));
		return *this;
	}

	// Accessors to the contiguous memory block.
	const char* frameData() const { return m_frame.data(); }
	size_t frameLength() const { return m_frame.size(); }

private:
	std::vector<char> m_frame; // Entire frame: header | payload | checksum.
	uint8_t m_msgType;
};

//----------------------------------------------------
// sendPacket() prints the entire frame in hex.
// It prints header fields and then the full contiguous memory block byte by byte.
void sendPacket(const NetworkPacket* packet) {
	std::cout << "Packet Header:\n";
	std::cout << "  Source      : " << packet->source() << "\n";
	std::cout << "  Destination : " << packet->dest() << "\n";
	std::cout << "  Sequence    : " << static_cast<int>(packet->sequenceNumber()) << "\n";
	std::cout << "  Checksum    : " << static_cast<int>(packet->checksum()) << "\n";
	std::cout << "  Message Type: " << static_cast<int>(packet->messageType()) << "\n";

	if (const ContiguousPacket* cp = dynamic_cast<const ContiguousPacket*>(packet)) {
		const char* data = cp->frameData();
		size_t len = cp->frameLength();
		std::cout << "Full frame (" << len << " bytes):\n";
		for (size_t i = 0; i < len; ++i) {
			std::cout << std::setw(2) << std::setfill('0')
				<< std::hex << (static_cast<unsigned int>(static_cast<unsigned char>(data[i]))) << " ";
		}
		std::cout << std::dec << std::endl;
	}
	else {
		std::cout << "Unknown packet type.\n";
	}
}

//----------------------------------------------------
// demo() builds a packet using chained header setters and payload writes.
// It demonstrates writing a payload byte directly (via writeUInt8),
// writing an external payload block, and then writing another byte.
// Finally, finalize() computes and appends the checksum.
void demo() {
	// Simulate an external payload buffer.
	std::vector<char> externalPayload = { static_cast<char>(0x12),
		static_cast<char>(0x34),
		static_cast<char>(0x56) };
	PacketBuffer externalBuffer = PacketBuffer::createReadOnly(
		reinterpret_cast<const unsigned char*>(externalPayload.data()),
		0,
		externalPayload.size());

	// Build the packet by chaining header setters and payload writes.
	ContiguousPacket packet;
	packet.setMessageType(1)
		.setSequenceNumber(42)
		.setSource("DeviceA")
		.setDest("DeviceB")
		.writeUInt8(0xAA)                          // Directly write a byte.
		.writePayload(externalBuffer, externalPayload.size())  // Write external payload block.
		.writeUInt8(0xFF)                          // Write an additional byte.
		.finalize();

	// "Send" the packet by printing its full continuous frame in hex.
	sendPacket(&packet);
}
/*
Packet Header:
Source      : DeviceA
Destination : DeviceB
Sequence    : 42
Checksum    : 13
Message Type: 1
Full frame (28 bytes):
01 2a 44 65 76 69 63 65 41 00 00 00 44 65 76 69 63 65 42 00 00 00 aa 12 34 56 ff 0d
*/


//int _tmain(int argc, _TCHAR* argv[]) //for visual studio
int main() {
{
	demo();
	return 0;
}
/*
int setPacketFrame(unsigned char* acData, unsigned char ucType) 
{
    acData[0] = ucType;
    acData[1] = getSequenceNum();  //
    snprintf(acData+2, 10, "%s", SOURCE_DEV); 
    snprintf(acData+12, 10, "%s", DEST_DEV); 
    acData[22] = getReq();
    acData[23] = getResult(); 
    acData[24] = getReason();
    acData[25] = getSomething1(); 
    acData[26] = getSomething2(); 
    acData[27] = calcBcc(acData+1, 12);
    return 28;
}
*/
