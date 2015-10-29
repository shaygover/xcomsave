#include "minilzo.h"
#include "xcomreader.h"
#include "util.h"
#include <string>
#include <memory>
#include <cassert>

uint32_t XComReader::readInt32()
{
	uint32_t v = *(reinterpret_cast<const uint32_t*>(ptr_));
	ptr_ += 4;
	return v;
}

float XComReader::readFloat()
{
	float f = *(reinterpret_cast<const float*>(ptr_));
	ptr_ += 4;
	return f;
}

bool XComReader::readBool()
{
	return readInt32() != 0;
}

const char* XComReader::readString()
{
	uint32_t len = readInt32();
	if (len == 0) {
		return "";
	}
	const char *str = reinterpret_cast<const char *>(ptr_);
	size_t actualLen = strlen(str);
	
	// Double check the length matches what we read from the file, considering the trailing null is counted in the length 
	// stored in the file.
	if (actualLen != (len-1))
	{
		fprintf(stderr, "Error: String mismatch at offset %d. Expected length %d but got %d\n", ptr_ - start_, len, actualLen);
		return "<error>";
	}

	ptr_ += len;
	return str;
}

XComSaveHeader XComReader::readHeader()
{
	XComSaveHeader hdr;
	hdr.version = readInt32();
	if (hdr.version != 0x10) {
		fprintf(stderr, "Error: Data does not appear to be an xcom save: expected file version 16 but got %d\n", hdr.version);
		return {0};
	}
	hdr.uncompressedSize = readInt32();
	hdr.gameNumber = readInt32();
	hdr.saveNumber = readInt32();
	hdr.saveDescription = readString();
	hdr.time = readString();
	hdr.mapCommand = readString();
	hdr.tacticalSave = readBool();
	hdr.ironman = readBool();
	hdr.autoSave = readBool();
	hdr.dlcString = readString();
	hdr.language = readString();
	hdr.crc = readInt32();
	return hdr;
}

XComActorTable XComReader::readActorTable()
{
	XComActorTable actorTable;
	uint32_t actorCount = readInt32();

	for (unsigned int i = 0; i < actorCount; ++i) {
		std::string name = readString();
		uint32_t instanceNum = readInt32();
		actorTable.push_back({name, instanceNum });
	}

	return actorTable;
}

XComPropertyList XComReader::readProperties(uint32_t dataLen)
{
	const unsigned char *endpos = ptr_ + dataLen;
	XComPropertyList properties;
	while (ptr_ < endpos)
	{
		std::string name = readString();
		uint32_t unknown1 = readInt32();
		if (unknown1 != 0) {
			DBG("Read non-zero prop unknown value: %x\n", unknown1);
		}

		if (name.compare("None") == 0) {
			break;
		}
		std::string propType = readString();
		uint32_t unknown2 = readInt32();
		if (unknown2 != 0) {
			DBG("Read non-zero prop unknown2 value: %x\n", unknown2);
		}
		uint32_t propSize = readInt32();
		uint32_t arrayIdx = readInt32();

		XComPropertyPtr prop;
		if (propType.compare("ObjectProperty") == 0) {
			std::vector<unsigned char> data;
			for (unsigned int i = 0; i < propSize; ++i) {
				data.push_back(*ptr_++);
			}
			prop = std::make_unique<XComObjectProperty>(name, data);
		}
		else if (propType.compare("IntProperty") == 0) {
			assert(propSize == 4);
			uint32_t intVal = readInt32();
			prop = std::make_unique<XComIntProperty>(name, intVal);
		}
		else if (propType.compare("ByteProperty") == 0) {
			std::string enumType = readString();
			uint32_t innerUnknown = readInt32();
			if (innerUnknown != 0) {
				DBG("Read non-zero prop unknown3 value: %x\n", innerUnknown);
			}
			std::string enumVal = readString();
			uint32_t extVal = readInt32();
			prop = std::make_unique<XComByteProperty>(name, enumType, enumVal, extVal);
		}
		else if (propType.compare("BoolProperty") == 0) {
			assert(propSize == 0);
			bool boolVal = *ptr_++ != 0;
			prop = std::make_unique<XComBoolProperty>(name, boolVal);
		}
		else if (propType.compare("ArrayProperty") == 0) {
			uint32_t arrayBound = readInt32();
			unsigned char *arrayData = nullptr;
			if (propSize > 4) {
				arrayData = new unsigned char[propSize - 4];
				memcpy(arrayData, ptr_, propSize - 4);
				ptr_ += (propSize - 4);
			}
			prop = std::make_unique<XComArrayProperty>(name, arrayData, arrayBound, propSize == 4 ? 0 : (propSize - 4)/arrayBound);
		}
		else if (propType.compare("FloatProperty") == 0) {
			float f = readFloat();
			prop = std::make_unique<XComFloatProperty>(name, f);
		}
		else if (propType.compare("StructProperty") == 0) {
			std::string structName = readString();
			uint32_t innerUnknown = readInt32();
			if (innerUnknown != 0) {
				DBG("Read non-zero prop unknown3 value: %x\n", innerUnknown);
			}
			// Special case certain structs
			if (structName.compare("Vector2D") == 0) {
				assert(propSize == 8);
				unsigned char* nativeData = new unsigned char[8];
				memcpy(nativeData, ptr_, 8);
				ptr_ += 8;
				prop = std::make_unique<XComStructProperty>(name, structName, nativeData, 8);
			} 
			else if (structName.compare("Vector") == 0) {
				assert(propSize == 12);
				unsigned char * nativeData = new unsigned char[12];
				memcpy(nativeData, ptr_, 12);
				ptr_ += 12;
				prop = std::make_unique<XComStructProperty>(name, structName, nativeData, 12);
			}
			else {
				XComPropertyList structProps = readProperties(propSize);
				prop = std::make_unique<XComStructProperty>(name, structName, std::move(structProps));
			}
		}
		else if (propType.compare("StrProperty") == 0) {
			std::string str = readString();
			prop = std::make_unique<XComStrProperty>(name, str);
		}
		else
		{
			DBG("Error: unknown property type %s\n", propType.c_str());
		}

		if (prop.get() != nullptr) {
			if (arrayIdx == 0) {
				properties.push_back(std::move(prop));
			}
			else {
				if (properties.back()->getName().compare(name) != 0) {
					DBG("Static array index found but doesn't match last property at offset 0x%x\n", ptr_ - start_);
				}

				if (properties.back()->getKind() == XComProperty::Kind::StaticArrayProperty) {
					// We already have a static array. Sanity check the array index and add it
					assert(arrayIdx == static_cast<XComStaticArrayProperty*>(properties.back().get())->size());
					static_cast<XComStaticArrayProperty*>(properties.back().get())->addProperty(std::move(prop));
				}
				else {
					// Not yet a static array. This new property should have index 1.
					assert(arrayIdx == 1);

					// Pop off the old property
					XComPropertyPtr lastProp = std::move(properties.back());
					properties.pop_back();

					// And replace it with a new static array
					std::unique_ptr<XComStaticArrayProperty> staticArrayProp = std::make_unique<XComStaticArrayProperty>(name);
					staticArrayProp->addProperty(std::move(lastProp));
					staticArrayProp->addProperty(std::move(prop));
					properties.push_back(std::move(staticArrayProp));
				}
			}
		}
	}

	return properties;
}

XComCheckpointTable XComReader::readCheckpointTable()
{
	XComCheckpointTable checkpointTable;
	uint32_t checkpointCount = readInt32();

	for (unsigned int i = 0; i < checkpointCount; ++i) {
		XComCheckpoint chk;
		chk.name = readString();
		chk.instanceName = readString();
		chk.vector[0] = readFloat();
		chk.vector[1] = readFloat();
		chk.vector[2] = readFloat();
		chk.rotator[0] = readFloat();
		chk.rotator[1] = readFloat();
		chk.rotator[2] = readFloat();
		chk.className = readString();
		uint32_t propLen = readInt32();
		const unsigned char* startPtr = ptr_;
		chk.properties = readProperties(propLen);
		if (uint32_t(ptr_ - startPtr) < propLen) {
			chk.padSize = propLen - (ptr_ - startPtr);

			for (unsigned int i = 0; i < chk.padSize; ++i) {
				if (*ptr_++ != 0) {
					DBG("Found non-zero padding byte at offset %x", (ptr_ - start_ - 1));
				}
			}
		}
		chk.templateIndex = readInt32();
		checkpointTable.push_back(std::move(chk));
	}

	return checkpointTable;
}

XComActorTemplateTable XComReader::readActorTemplateTable()
{
	XComActorTemplateTable templateTable;
	uint32_t templateCount = readInt32();

	for (unsigned int i = 0; i < templateCount; ++i) {
		XComActorTemplate tmpl;
		tmpl.actorClassPath = readString();
		memcpy(tmpl.loadParams, ptr_, 64);
		ptr_ += 64;
		tmpl.archetypePath = readString();
		templateTable.push_back(std::move(tmpl));
	}

	return templateTable;
}

XComNameTable XComReader::readNameTable()
{
	static unsigned char allZeros[8] = { 0 };
	XComNameTable nameTable;
	nameTable.nameCount = readInt32();
	nameTable.names = new XComNameEntry[nameTable.nameCount];

	for (unsigned int i = 0; i < nameTable.nameCount; ++i) {
		XComNameEntry *entry = &nameTable.names[i];
		entry->name = readString();
		memcpy(entry->zeros, ptr_, 8);
		ptr_ += 8;
		if (memcmp(entry->zeros, allZeros, 8) != 0) {
			fprintf(stderr, "Error: Expected all zeros in name table entry");
			return{};
		}
		entry->dataLen = readInt32();
		entry->data = new unsigned char[entry->dataLen];
		memcpy(entry->data, ptr_, entry->dataLen);
		ptr_ += entry->dataLen;
	}

	return nameTable;
}

int32_t XComReader::getUncompressedSize()
{
	// The compressed data begins 1024 bytes into the file.
	const unsigned char* p = start_ + 1024;
	uint32_t compressedSize;
	uint32_t uncompressedSize = 0;
	do
	{
		// Expect the magic header value 0x9e2a83c1 at the start of each chunk
		if (*(reinterpret_cast<const uint32_t*>(p)) != 0x9e2a83c1) {
			fprintf(stderr, "Failed to find compressed chunk at offset 0x%08x", (p - start_));
			return -1;
		}

		// Compressed size is at p+8
		compressedSize = *(reinterpret_cast<const unsigned long*>(p + 8));

		// Uncompressed size is at p+12
		uncompressedSize += *(reinterpret_cast<const unsigned long*>(p + 12));

		// Skip to next chunk - 24 bytes of this chunk header + compressedSize bytes later.
		p += (compressedSize + 24);
	} while (p - start_ < length_);

	return uncompressedSize;
}

void XComReader::getUncompressedData(unsigned char *buf)
{
	// Start back at the beginning of the compressed data.
	const unsigned char *p = start_ + 1024;
	unsigned char *outp = buf;
	do
	{
		// Expect the magic header value 0x9e2a83c1 at the start of each chunk
		if (*(reinterpret_cast<const uint32_t*>(p)) != 0x9e2a83c1) {
			fprintf(stderr, "Failed to find compressed chunk at offset 0x%08x", (p - start_));
			return;
		}

		// Compressed size is at p+8
		uint32_t compressedSize = *(reinterpret_cast<const unsigned long*>(p + 8));

		// Uncompressed size is at p+12
		uint32_t uncompressedSize = *(reinterpret_cast<const unsigned long*>(p + 12));
		
		unsigned long decompSize = uncompressedSize;

		if (lzo1x_decompress_safe(p + 24, compressedSize, outp, &decompSize, nullptr) != LZO_E_OK) {
			fprintf(stderr, "LZO decompress of save data failed\n");
			return;
		}

		if (decompSize != uncompressedSize)
		{
			fprintf(stderr, "Failed to decompress chunk!");
			return;
		}

		// Skip to next chunk - 24 bytes of this chunk header + compressedSize bytes later.
		p += (compressedSize + 24);
		outp += uncompressedSize;
	} while (p - start_ < length_);
}

XComSave XComReader::getSaveData()
{
	XComSave save;
	const unsigned char *p = ptr_ + 1024;
	FILE *outFile = fopen("output.dat", "wb");
	if (outFile == nullptr) {
		fprintf(stderr, "Failed to open output file: %d", errno);
	}
	int chunkCount = 0;
	int totalCompressed = 0;
	int totalUncompressed = 0;
	save.header = readHeader();

	uint32_t uncompressedSize = getUncompressedSize();
	if (uncompressedSize < 0) {
		return {};
	}
	
	unsigned char *uncompressedData = new unsigned char[uncompressedSize];
	getUncompressedData(uncompressedData);

	// We're now done with the compressed file. Swap over to the uncompressed data
	start_ = ptr_ = uncompressedData;
	length_ = uncompressedSize;
	fwrite(uncompressedData, 1, uncompressedSize, outFile);
	fclose(outFile);
	save.actorTable = readActorTable();
	DBG("Finished reading actor table at offset %x\n", ptr_ - start_);
	// Jump back to here after each chunk
	do {
		XComCheckpointChunk chunk;
		chunk.unknownInt1 = readInt32();
		chunk.unknownString1 = readString();
		const char *none = readString();
		if (strcmp(none, "None") != 0) {
			fprintf(stderr, "Error locating 'None' after actor table.\n");
			return{};
		}
		
		chunk.unknownInt2 = readInt32();
		chunk.checkpointTable = readCheckpointTable();
		DBG("Finished reading checkpoint table at offset %x\n", ptr_ - start_);

		uint32_t nameTableLen = readInt32();
		assert(nameTableLen == 0);

#if 0
		if (unknownInt3 > 0) { // only seems to be present for tactical saves?
			XComNameTable nameTable = readNameTable();
			DBG("Finished reading name table at offset %x\n", ptr_ - start_);
		}
#endif

		chunk.unknownString2 = readString();
		chunk.actorTable = readActorTable();
		DBG("Finished reading second actor table at offset %x\n", ptr_ - start_);

		chunk.unknownInt3 = readInt32();

		XComActorTemplateTable actorTemplateTable = readActorTemplateTable(); // (only seems to be present for tactical saves?)
		assert(actorTemplateTable.size() == 0);
		DBG("Finished reading actor template table at offset %x\n", ptr_ - start_);

		chunk.gameName = readString(); //unknown (game name)
		chunk.mapName = readString(); //unknown (map name)
		chunk.unknownInt4 = readInt32(); //unknown  (checksum?)

		save.checkpoints.push_back(std::move(chunk));
	}
	while ((ptr_ - start_ < length_));
	return save;
}