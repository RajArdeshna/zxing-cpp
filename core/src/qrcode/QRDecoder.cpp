/*
* Copyright 2016 Nu-book Inc.
* Copyright 2016 ZXing authors
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "qrcode/QRDecoder.h"
#include "qrcode/QRBitMatrixParser.h"
#include "qrcode/QRVersion.h"
#include "qrcode/QRFormatInformation.h"
#include "qrcode/QRDecoderMetadata.h"
#include "qrcode/QRDataMask.h"
#include "qrcode/QRDataBlock.h"
#include "qrcode/QRDecodeMode.h"
#include "DecoderResult.h"
#include "BitMatrix.h"
#include "ReedSolomonDecoder.h"
#include "GenericGF.h"
#include "BitSource.h"
#include "TextDecoder.h"
#include "CharacterSet.h"
#include "CharacterSetECI.h"
#include "DecodeHints.h"
#include "ErrorStatus.h"

#include <list>
#include <type_traits>

namespace ZXing {
namespace QRCode {

/**
* <p>Given data and error-correction codewords received, possibly corrupted by errors, attempts to
* correct the errors in-place using Reed-Solomon error correction.</p>
*
* @param codewordBytes data and error correction codewords
* @param numDataCodewords number of codewords that are data bytes
* @throws ChecksumException if error correction fails
*/
static ErrorStatus
CorrectErrors(ByteArray& codewordBytes, int numDataCodewords)
{
	int numCodewords = codewordBytes.length();
	// First read into an array of ints
	std::vector<int> codewordsInts(numCodewords);
	for (int i = 0; i < numCodewords; i++) {
		codewordsInts[i] = codewordBytes[i] & 0xFF;
	}
	int numECCodewords = codewordBytes.length() - numDataCodewords;
	auto status = ReedSolomonDecoder(GenericGF::QRCodeField256()).decode(codewordsInts, numECCodewords);
	if (StatusIsOK(status))
	{
		// Copy back into array of bytes -- only need to worry about the bytes that were data
		// We don't care about errors in the error-correction codewords
		for (int i = 0; i < numDataCodewords; ++i) {
			codewordBytes[i] = static_cast<uint8_t>(codewordsInts[i]);
		}
	}
	else if (StatusIsKindOf(status, ErrorStatus::ReedSolomonError))
	{
		status = ErrorStatus::ChecksumError;
	}
	return status;
}


/**
* See specification GBT 18284-2000
*/
static ErrorStatus
DecodeHanziSegment(BitSource& bits, int count, std::wstring& result)
{
	// Don't crash trying to read more bits than we have available.
	if (count * 13 > bits.available()) {
		return ErrorStatus::FormatError;
	}

	// Each character will require 2 bytes. Read the characters as 2-byte pairs
	// and decode as GB2312 afterwards
	ByteArray buffer;
	buffer.reserve(2 * count);
	while (count > 0) {
		// Each 13 bits encodes a 2-byte character
		int twoBytes = bits.readBits(13);
		int assembledTwoBytes = ((twoBytes / 0x060) << 8) | (twoBytes % 0x060);
		if (assembledTwoBytes < 0x003BF) {
			// In the 0xA1A1 to 0xAAFE range
			assembledTwoBytes += 0x0A1A1;
		}
		else {
			// In the 0xB0A1 to 0xFAFE range
			assembledTwoBytes += 0x0A6A1;
		}
		buffer.push_back(static_cast<uint8_t>((assembledTwoBytes >> 8) & 0xFF));
		buffer.push_back(static_cast<uint8_t>(assembledTwoBytes & 0xFF));
		count--;
	}

	TextDecoder::Append(result, buffer.data(), buffer.length(), CharacterSet::GB2312);
	return ErrorStatus::NoError;
}

static ErrorStatus
DecodeKanjiSegment(BitSource& bits, int count, std::wstring& result)
{
	// Don't crash trying to read more bits than we have available.
	if (count * 13 > bits.available()) {
		return ErrorStatus::FormatError;
	}

	// Each character will require 2 bytes. Read the characters as 2-byte pairs
	// and decode as Shift_JIS afterwards
	ByteArray buffer;
	buffer.reserve(2 * count);
	while (count > 0) {
		// Each 13 bits encodes a 2-byte character
		int twoBytes = bits.readBits(13);
		int assembledTwoBytes = ((twoBytes / 0x0C0) << 8) | (twoBytes % 0x0C0);
		if (assembledTwoBytes < 0x01F00) {
			// In the 0x8140 to 0x9FFC range
			assembledTwoBytes += 0x08140;
		}
		else {
			// In the 0xE040 to 0xEBBF range
			assembledTwoBytes += 0x0C140;
		}
		buffer.push_back(static_cast<uint8_t>(assembledTwoBytes >> 8));
		buffer.push_back(static_cast<uint8_t>(assembledTwoBytes));
		count--;
	}

	TextDecoder::Append(result, buffer.data(), buffer.length(), CharacterSet::Shift_JIS);
	return ErrorStatus::NoError;
}

static ErrorStatus
DecodeByteSegment(BitSource& bits, int count, CharacterSet currentCharset, const std::string& hintedCharset, std::wstring& result, std::list<ByteArray>& byteSegments)
{
	// Don't crash trying to read more bits than we have available.
	if (8 * count > bits.available()) {
		return ErrorStatus::FormatError;
	}

	ByteArray readBytes(count);
	for (int i = 0; i < count; i++) {
		readBytes[i] = static_cast<uint8_t>(bits.readBits(8));
	}
 	if (currentCharset == CharacterSet::Unknown) {
		// The spec isn't clear on this mode; see
		// section 6.4.5: t does not say which encoding to assuming
		// upon decoding. I have seen ISO-8859-1 used as well as
		// Shift_JIS -- without anything like an ECI designator to
		// give a hint.
		if (!hintedCharset.empty())
		{
			currentCharset = CharacterSetECI::CharsetFromName(hintedCharset.c_str());
		}
		if (currentCharset == CharacterSet::Unknown)
		{
			currentCharset = TextDecoder::GuessEncoding(readBytes.data(), readBytes.length());
		}
	}
	TextDecoder::Append(result, readBytes.data(), readBytes.length(), currentCharset);
	byteSegments.push_back(readBytes);
	return ErrorStatus::NoError;
}

static char
ToAlphaNumericChar(int value)
{
	/**
	* See ISO 18004:2006, 6.4.4 Table 5
	*/
	static const char ALPHANUMERIC_CHARS[] = {
		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B',
		'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N',
		'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
		' ', '$', '%', '*', '+', '-', '.', '/', ':'
	};

	if (value < 0 || value >= (int)std::extent<decltype(ALPHANUMERIC_CHARS)>::value) {
		throw std::out_of_range("ToAlphaNumericChar: out of range");
	}
	return ALPHANUMERIC_CHARS[value];
}

static ErrorStatus
DecodeAlphanumericSegment(BitSource& bits, int count, bool fc1InEffect, std::wstring& result)
{
	// Read two characters at a time
	std::string buffer;
	while (count > 1) {
		if (bits.available() < 11) {
			return ErrorStatus::FormatError;
		}
		int nextTwoCharsBits = bits.readBits(11);
		buffer += ToAlphaNumericChar(nextTwoCharsBits / 45);
		buffer += ToAlphaNumericChar(nextTwoCharsBits % 45);
		count -= 2;
	}
	if (count == 1) {
		// special case: one character left
		if (bits.available() < 6) {
			return ErrorStatus::FormatError;
		}
		buffer += ToAlphaNumericChar(bits.readBits(6));
	}
	// See section 6.4.8.1, 6.4.8.2
	if (fc1InEffect) {
		// We need to massage the result a bit if in an FNC1 mode:
		for (size_t i = 0; i < buffer.length(); i++) {
			if (buffer[i] == '%') {
				if (i < buffer.length() - 1 && buffer[i + 1] == '%') {
					// %% is rendered as %
					buffer.erase(i + 1);
				}
				else {
					// In alpha mode, % should be converted to FNC1 separator 0x1D
					buffer[i] = static_cast<char>(0x1D);
				}
			}
		}
	}
	TextDecoder::AppendLatin1(result, buffer);
	return ErrorStatus::NoError;
}

static ErrorStatus
DecodeNumericSegment(BitSource& bits, int count, std::wstring& result)
{
	// Read three digits at a time
	std::string buffer;
	while (count >= 3) {
		// Each 10 bits encodes three digits
		if (bits.available() < 10) {
			return ErrorStatus::FormatError;
		}
		int threeDigitsBits = bits.readBits(10);
		if (threeDigitsBits >= 1000) {
			return ErrorStatus::FormatError;
		}
		buffer += ToAlphaNumericChar(threeDigitsBits / 100);
		buffer += ToAlphaNumericChar((threeDigitsBits / 10) % 10);
		buffer += ToAlphaNumericChar(threeDigitsBits % 10);
		count -= 3;
	}
	if (count == 2) {
		// Two digits left over to read, encoded in 7 bits
		if (bits.available() < 7) {
			return ErrorStatus::FormatError;
		}
		int twoDigitsBits = bits.readBits(7);
		if (twoDigitsBits >= 100) {
			return ErrorStatus::FormatError;
		}
		buffer += ToAlphaNumericChar(twoDigitsBits / 10);
		buffer += ToAlphaNumericChar(twoDigitsBits % 10);
	}
	else if (count == 1) {
		// One digit left over to read
		if (bits.available() < 4) {
			return ErrorStatus::FormatError;
		}
		int digitBits = bits.readBits(4);
		if (digitBits >= 10) {
			return ErrorStatus::FormatError;
		}
		buffer += ToAlphaNumericChar(digitBits);
	}

	TextDecoder::AppendLatin1(result, buffer);
	return ErrorStatus::NoError;
}

static ErrorStatus
ParseECIValue(BitSource& bits, int &outValue)
{
	int firstByte = bits.readBits(8);
	if ((firstByte & 0x80) == 0) {
		// just one byte
		outValue = firstByte & 0x7F;
		return ErrorStatus::NoError;
	}
	if ((firstByte & 0xC0) == 0x80) {
		// two bytes
		int secondByte = bits.readBits(8);
		outValue = ((firstByte & 0x3F) << 8) | secondByte;
		return ErrorStatus::NoError;
	}
	if ((firstByte & 0xE0) == 0xC0) {
		// three bytes
		int secondThirdBytes = bits.readBits(16);
		outValue = ((firstByte & 0x1F) << 16) | secondThirdBytes;
		return ErrorStatus::NoError;
	}
	return ErrorStatus::FormatError;
}

/**
* <p>QR Codes can encode text as bits in one of several modes, and can use multiple modes
* in one QR Code. This method decodes the bits back into text.</p>
*
* <p>See ISO 18004:2006, 6.4.3 - 6.4.7</p>
*/
static ErrorStatus
DecodeBitStream(const ByteArray& bytes, const Version& version, ErrorCorrectionLevel ecLevel, const std::string& hintedCharset, DecoderResult& decodeResult)
{
	BitSource bits(bytes);
	std::wstring result;
	std::list<ByteArray> byteSegments;
	int symbolSequence = -1;
	int parityData = -1;
	static const int GB2312_SUBSET = 1;

	try
	{
		CharacterSet currentCharset = CharacterSet::Unknown;
		bool fc1InEffect = false;
		DecodeMode::Mode mode;
		do {
			// While still another segment to read...
			if (bits.available() < 4) {
				// OK, assume we're done. Really, a TERMINATOR mode should have been recorded here
				mode = DecodeMode::TERMINATOR;
			}
			else {
				mode = DecodeMode::ModeForBits(bits.readBits(4)); // mode is encoded by 4 bits
			}
			if (mode != DecodeMode::TERMINATOR) {
				if (mode == DecodeMode::FNC1_FIRST_POSITION || mode == DecodeMode::FNC1_SECOND_POSITION) {
					// We do little with FNC1 except alter the parsed result a bit according to the spec
					fc1InEffect = true;
				}
				else if (mode == DecodeMode::STRUCTURED_APPEND) {
					if (bits.available() < 16) {
						return ErrorStatus::FormatError;
					}
					// sequence number and parity is added later to the result metadata
					// Read next 8 bits (symbol sequence #) and 8 bits (parity data), then continue
					symbolSequence = bits.readBits(8);
					parityData = bits.readBits(8);
				}
				else if (mode == DecodeMode::ECI) {
					// Count doesn't apply to ECI
					int value;
					auto status = ParseECIValue(bits, value);
					if (StatusIsError(status)) {
						return status;
					}
					currentCharset = CharacterSetECI::CharsetFromValue(value);
					if (currentCharset == CharacterSet::Unknown) {
						return ErrorStatus::FormatError;
					}
				}
				else {
					// First handle Hanzi mode which does not start with character count
					if (mode == DecodeMode::HANZI) {
						//chinese mode contains a sub set indicator right after mode indicator
						int subset = bits.readBits(4);
						int countHanzi = bits.readBits(DecodeMode::CharacterCountBits(mode, version));
						if (subset == GB2312_SUBSET) {
							auto status = DecodeHanziSegment(bits, countHanzi, result);
							if (StatusIsError(status)) {
								return status;
							}
						}
					}
					else {
						// "Normal" QR code modes:
						// How many characters will follow, encoded in this mode?
						int count = bits.readBits(DecodeMode::CharacterCountBits(mode, version));
						ErrorStatus status;
						if (mode == DecodeMode::NUMERIC) {
							status = DecodeNumericSegment(bits, count, result);
						}
						else if (mode == DecodeMode::ALPHANUMERIC) {
							status = DecodeAlphanumericSegment(bits, count, fc1InEffect, result);
						}
						else if (mode == DecodeMode::BYTE) {
							status = DecodeByteSegment(bits, count, currentCharset, hintedCharset, result, byteSegments);
						}
						else if (mode == DecodeMode::KANJI) {
							status = DecodeKanjiSegment(bits, count, result);
						}
						else {
							status = ErrorStatus::FormatError;
						}

						if (StatusIsError(status)) {
							return status;
						}
					}
				}
			}
		} while (mode != DecodeMode::TERMINATOR);
	}
	catch (const std::exception &)
	{
		// from readBits() calls
		return ErrorStatus::FormatError;
	}
	
	decodeResult.setRawBytes(bytes);
	decodeResult.setText(result);
	decodeResult.setByteSegments(byteSegments);
	decodeResult.setEcLevel(ToString(ecLevel));
	decodeResult.setStructuredAppendSequenceNumber(symbolSequence);
	decodeResult.setStructuredAppendParity(parityData);
	return ErrorStatus::NoError;
}


static ErrorStatus
DoDecode(const BitMatrix& bits, const Version& version, const FormatInformation& formatInfo, const std::string& hintedCharset, DecoderResult& result)
{
	auto ecLevel = formatInfo.errorCorrectionLevel();

	// Read codewords
	ByteArray codewords;
	ErrorStatus status = BitMatrixParser::ReadCodewords(bits, version, codewords);
	if (StatusIsError(status)) {
		return status;
	}
	// Separate into data blocks
	std::vector<DataBlock> dataBlocks;
	status = DataBlock::GetDataBlocks(codewords, version, ecLevel, dataBlocks);
	if (StatusIsError(status)) {
		return status;
	}
	// Count total number of data bytes
	int totalBytes = 0;
	for (const auto& dataBlock : dataBlocks) {
		totalBytes += dataBlock.numDataCodewords();
	}
	ByteArray resultBytes(totalBytes);
	int resultOffset = 0;

	// Error-correct and copy data blocks together into a stream of bytes
	for (auto& dataBlock : dataBlocks)
	{
		ByteArray& codewordBytes = dataBlock.codewords();
		int numDataCodewords = dataBlock.numDataCodewords();
		
		status = CorrectErrors(codewordBytes, numDataCodewords);
		if (StatusIsError(status)) {
			return status;
		}
		for (int i = 0; i < numDataCodewords; i++) {
			resultBytes[resultOffset++] = codewordBytes[i];
		}
	}

	// Decode the contents of that stream of bytes
	return DecodeBitStream(resultBytes, version, ecLevel, hintedCharset, result);
}

static void
ReMask(BitMatrix& bitMatrix, const FormatInformation& formatInfo)
{
	int dimension = bitMatrix.height();
	DataMask(formatInfo.dataMask()).unmaskBitMatrix(bitMatrix, dimension);
}


ErrorStatus
Decoder::Decode(const BitMatrix& bits_, const std::string& hintedCharset, DecoderResult& result)
{
	BitMatrix bits;
	bits_.copyTo(bits);
	// Construct a parser and read version, error-correction level
	const Version* version;
	FormatInformation formatInfo;

	ErrorStatus status = BitMatrixParser::ParseVersionInfo(bits, false, version, formatInfo);
	if (StatusIsOK(status))
	{
		ReMask(bits, formatInfo);
		status = DoDecode(bits, *version, formatInfo, hintedCharset, result);
		if (StatusIsOK(status)) {
			return status;
		}
	}

	if (version != nullptr)
	{
		// Revert the bit matrix
		ReMask(bits, formatInfo);
	}

	status = BitMatrixParser::ParseVersionInfo(bits, true, version, formatInfo);
	if (StatusIsOK(status))
	{
		/*
		* Since we're here, this means we have successfully detected some kind
		* of version and format information when mirrored. This is a good sign,
		* that the QR code may be mirrored, and we should try once more with a
		* mirrored content.
		*/
		// Prepare for a mirrored reading.
		bits.mirror();

		ReMask(bits, formatInfo);
		status = DoDecode(bits, *version, formatInfo, hintedCharset, result);
		if (StatusIsOK(status))
		{
			result.setExtra(std::make_shared<DecoderMetadata>(true));
		}
	}
	return status;
}

} // QRCode
} // ZXing