/*
 *  Copyright (c) 2010,
 *  Gavriloaie Eugen-Andrei (shiretu@gmail.com)
 *
 *  This file is part of crtmpserver.
 *  crtmpserver is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  crtmpserver is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with crtmpserver.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "streaming/baseinfilestream.h"
#include "streaming/basestream.h"
#include "streaming/baseoutstream.h"
#include "streaming/streamstypes.h"
#include "protocols/baseprotocol.h"
#include "mediaformats/basemediadocument.h"
#include "mediaformats/flv/flvdocument.h"
#include "mediaformats/mp3/mp3document.h"
#include "mediaformats/mp4/mp4document.h"
#include "mediaformats/nsv/nsvdocument.h"
#include "application/baseclientapplication.h"

#ifndef HAS_MMAP
map<string, pair<uint32_t, File *> > BaseInFileStream::_fileCache;
#endif /* HAS_MMAP */

BaseInFileStream::InFileStreamTimer::InFileStreamTimer(BaseInFileStream *pInFileStream) {
	_pInFileStream = pInFileStream;
}

BaseInFileStream::InFileStreamTimer::~InFileStreamTimer() {

}

void BaseInFileStream::InFileStreamTimer::ResetStream() {
	_pInFileStream = NULL;
}

bool BaseInFileStream::InFileStreamTimer::TimePeriodElapsed() {
	if (_pInFileStream != NULL)
		_pInFileStream->ReadyForSend();
	return true;
}

#define FILE_STREAMING_STATE_PAUSED 0
#define FILE_STREAMING_STATE_PLAYING 1
#define FILE_STREAMING_STATE_FINISHED 2

BaseInFileStream::BaseInFileStream(BaseProtocol *pProtocol,
		StreamsManager *pStreamsManager, uint64_t type, string name)
: BaseInStream(pProtocol, pStreamsManager, type, name) {
	if (!TAG_KIND_OF(type, ST_IN_FILE)) {
		ASSERT("Incorrect stream type. Wanted a stream type in class %s and got %s",
				STR(tagToString(ST_IN_FILE)), STR(tagToString(type)));
	}
	_pTimer = NULL;
	_pSeekFile = NULL;
	_pFile = NULL;

	//frame info
	_totalFrames = 0;
	_currentFrameIndex = 0;
	memset(&_currentFrame, 0, sizeof (MediaFrame));

	//timing info
	_totalSentTime = 0;
	_totalSentTimeBase = 0;
	_startFeedingTime = 0;

	//buffering info
	_clientSideBufferLength = 0;

	//current state info
	_streamingState = FILE_STREAMING_STATE_PAUSED;
	_audioVideoCodecsSent = false;

	_seekBaseOffset = 0;
	_framesBaseOffset = 0;
	_timeToIndexOffset = 0;

	_streamCapabilities.Clear();

	_playLimit = -1;

#ifdef HAS_VOD_MANAGER
	_servedBytes = 0;
	_mediaFileSize = 0;
#endif /* HAS_VOD_MANAGER */
}

BaseInFileStream::~BaseInFileStream() {
	if (_pTimer != NULL) {
		_pTimer->ResetStream();
		_pTimer->EnqueueForDelete();
		_pTimer = NULL;
	}
#ifdef HAS_VOD_MANAGER
	UpdateServedBytesInfo();
#endif /* HAS_VOD_MANAGER */
	ReleaseFile(_pSeekFile);
	ReleaseFile(_pFile);
}

void BaseInFileStream::SetClientSideBuffer(uint32_t value) {
	if (value == 0) {
		//WARN("Invalid client side buffer value: %"PRIu32, value);
		return;
	}
	if (value > 120) {
		value = 120;
	}
	if (_clientSideBufferLength > value) {
		//WARN("Client side buffer must be bigger than %"PRIu32, _clientSideBufferLength);
		return;
	}
	//	FINEST("Client side buffer modified: %"PRIu32" -> %"PRIu32,
	//			_clientSideBufferLength, value);
	_clientSideBufferLength = value;
}

uint32_t BaseInFileStream::GetClientSideBuffer() {
	return _clientSideBufferLength;
}

bool BaseInFileStream::StreamCompleted() {
	if (_currentFrameIndex >= _totalFrames)
		return true;
	if ((_playLimit >= 0) && ((_playLimit < (double) _totalSentTime)))
		return true;
	return false;
}

StreamCapabilities * BaseInFileStream::GetCapabilities() {
	return &_streamCapabilities;
}

bool BaseInFileStream::ResolveCompleteMetadata(Variant &metaData) {
	if ((bool)metaData[CONF_APPLICATION_EXTERNSEEKGENERATOR])
		return false;
	//1. Create the document
	BaseMediaDocument *pDocument = NULL;
	if (false) {

	}
#ifdef HAS_MEDIA_FLV
	else if (metaData[META_MEDIA_TYPE] == MEDIA_TYPE_FLV ||
			metaData[META_MEDIA_TYPE] == MEDIA_TYPE_LIVE_OR_FLV) {
		pDocument = new FLVDocument(metaData);
	}
#endif /* HAS_MEDIA_FLV */
#ifdef HAS_MEDIA_MP3
	else if (metaData[META_MEDIA_TYPE] == MEDIA_TYPE_MP3) {
		pDocument = new MP3Document(metaData);
	}
#endif /* HAS_MEDIA_MP3 */
#ifdef HAS_MEDIA_MP4
	else if (metaData[META_MEDIA_TYPE] == MEDIA_TYPE_MP4
			|| metaData[META_MEDIA_TYPE] == MEDIA_TYPE_M4A
			|| metaData[META_MEDIA_TYPE] == MEDIA_TYPE_M4V
			|| metaData[META_MEDIA_TYPE] == MEDIA_TYPE_MOV
			|| metaData[META_MEDIA_TYPE] == MEDIA_TYPE_F4V) {
		pDocument = new MP4Document(metaData);
	}
#endif /* HAS_MEDIA_MP4 */
#ifdef HAS_MEDIA_NSV
	else if (metaData[META_MEDIA_TYPE] == MEDIA_TYPE_NSV) {
		pDocument = new NSVDocument(metaData);
	}
#endif /* HAS_MEDIA_NSV */

	else {
		FATAL("File type not supported yet. Partial metadata:\n%s",
				STR(metaData.ToString()));
		return false;
	}

	//2. Process the document
	INFO("Generate seek/meta files for `%s`", STR(metaData[META_SERVER_FULL_PATH]));
	if (!pDocument->Process()) {
		FATAL("Unable to process document");
		delete pDocument;
		if ((bool)metaData[CONF_APPLICATION_RENAMEBADFILES]) {
			moveFile(metaData[META_SERVER_FULL_PATH],
					(string) metaData[META_SERVER_FULL_PATH] + ".bad");
		} else {
			WARN("File %s will not be renamed",
					STR(metaData[META_SERVER_FULL_PATH]));
		}
		return false;
	}

	//3. Get the medatada
	metaData = pDocument->GetMetadata();

	//4. cleanup
	delete pDocument;

	//5. Done
	return true;
}

#ifdef HAS_VOD_MANAGER

bool BaseInFileStream::Initialize(Variant &medatada, int32_t clientSideBufferLength,
		bool hasTimer) {
	//1. Check to see if we have an universal seeking file
	string seekFilePath = medatada[META_MEDIA_FILE_PATHS][META_MEDIA_SEEK];
	if (!fileExists(seekFilePath)) {
		FATAL("Invalid seek file %s", STR(seekFilePath));
		return false;
	}

	string metaFilePath = medatada[META_MEDIA_FILE_PATHS][META_MEDIA_SEEK];
	if (!fileExists(metaFilePath)) {
		FATAL("Invalid meta file %s", STR(metaFilePath));
		return false;
	}

	_infoFilePath = (string) medatada[META_MEDIA_FILE_PATHS][META_MEDIA_INFO];
	_filePaths = medatada[META_MEDIA_FILE_PATHS];

	//2. either open the origin or the cached file
	string mediaFilePath = medatada[META_MEDIA_FILE_PATHS][META_MEDIA_CACHE];
	if (mediaFilePath == "")
		mediaFilePath = (string) medatada[META_MEDIA_FILE_PATHS][META_MEDIA_ORIGIN];
	if (!fileExists(mediaFilePath)) {
		FATAL("Invalid media file %s", STR(mediaFilePath));
		return false;
	}

	//2. Open the seek file
	_pSeekFile = GetFile(seekFilePath, 128 * 1024);
	if (_pSeekFile == NULL) {
		FATAL("Unable to open seeking file %s", STR(seekFilePath));
		return false;
	}

	//3. read stream capabilities
	uint32_t streamCapabilitiesSize = 0;
	IOBuffer raw;
	//	if(!_pSeekFile->SeekBegin()){
	//		FATAL("Unable to seek to the beginning og the file");
	//		return false;
	//	}
	//
	if (!_pSeekFile->ReadUI32(&streamCapabilitiesSize, false)) {
		FATAL("Unable to read stream Capabilities Size");
		return false;
	}
	if (!raw.ReadFromFs(*_pSeekFile, streamCapabilitiesSize)) {
		FATAL("Unable to read raw stream Capabilities");
		return false;
	}
	if (!StreamCapabilities::Deserialize(raw, _streamCapabilities)) {
		FATAL("Unable to deserialize stream Capabilities. Please delete %s and %s files so they can be regenerated",
				STR(seekFilePath),
				STR(metaFilePath));
		return false;
	}

	//4. compute offsets
	_seekBaseOffset = _pSeekFile->Cursor();
	_framesBaseOffset = _seekBaseOffset + 4;


	//5. Compute the optimal window size by reading the biggest frame size
	//from the seek file.
	if (!_pSeekFile->SeekTo(_pSeekFile->Size() - 8)) {
		FATAL("Unable to seek to %"PRIu64" position", _pSeekFile->Cursor() - 8);
		return false;
	}
	uint64_t maxFrameSize = 0;
	if (!_pSeekFile->ReadUI64(&maxFrameSize, false)) {
		FATAL("Unable to read max frame size");
		return false;
	}
	if (!_pSeekFile->SeekBegin()) {
		FATAL("Unable to seek to beginning of the file");
		return false;
	}

	//3. Open the media file
	uint32_t windowSize = (uint32_t) maxFrameSize * 16;
	windowSize = windowSize < 65536 ? 65536 : windowSize;
	windowSize = (windowSize > (1024 * 1024)) ? (windowSize / 2) : windowSize;
	_pFile = GetFile(mediaFilePath, windowSize);
	if (_pFile == NULL) {
		FATAL("Unable to initialize file");
		return false;
	}
	_mediaFileSize = _pFile->Size();

	//4. Read the frames count from the file
	if (!_pSeekFile->SeekTo(_seekBaseOffset)) {
		FATAL("Unable to seek to _seekBaseOffset: %"PRIu64, _seekBaseOffset);
		return false;
	}
	if (!_pSeekFile->ReadUI32(&_totalFrames, false)) {
		FATAL("Unable to read the frames count");
		return false;
	}
	_timeToIndexOffset = _framesBaseOffset + _totalFrames * sizeof (MediaFrame);

	//5. Set the client side buffer length
	_clientSideBufferLength = clientSideBufferLength;

	//6. Create the timer
	if (hasTimer) {
		_pTimer = new InFileStreamTimer(this);
		double clientSideBufferDenominator = 0;
		if (_pProtocol->GetApplication()->GetConfiguration().HasKeyChain(_V_NUMERIC, false, 1, "clientSideBufferDenominator"))
			clientSideBufferDenominator = (double) _pProtocol->GetApplication()->GetConfiguration().GetValue("clientSideBufferDenominator", false);
		if (clientSideBufferDenominator <= 0)
			clientSideBufferDenominator = 3;
		double val = _clientSideBufferLength / clientSideBufferDenominator;
		if (val < 1)
			val = 1;
		if (val > (_clientSideBufferLength - 1))
			val = _clientSideBufferLength - 1;
		FINEST("_clientSideBufferLength: %"PRIu32"; timer: %"PRIu32, _clientSideBufferLength, (uint32_t) val);
		_pTimer->EnqueueForTimeEvent((uint32_t) val);
	}

	UpdateOpenCountInfo();

	//7. Done
	return true;
}
#else /* HAS_VOD_MANAGER */

bool BaseInFileStream::Initialize(int32_t clientSideBufferLength, bool hasTimer) {
	//1. Check to see if we have an universal seeking file
	string seekFilePath = GetName() + "." MEDIA_TYPE_SEEK;
	if (!fileExists(seekFilePath)) {
		Variant temp;
		temp[META_SERVER_FULL_PATH] = GetName();
		if (!ResolveCompleteMetadata(temp)) {
			FATAL("Unable to generate metadata");
			return false;
		}
	}

	//2. Open the seek file
	_pSeekFile = GetFile(seekFilePath, 128 * 1024);
	if (_pSeekFile == NULL) {
		FATAL("Unable to open seeking file %s", STR(seekFilePath));
		return false;
	}

	//3. read stream capabilities
	uint32_t streamCapabilitiesSize = 0;
	IOBuffer raw;
	//	if(!_pSeekFile->SeekBegin()){
	//		FATAL("Unable to seek to the beginning og the file");
	//		return false;
	//	}
	//
	if (!_pSeekFile->ReadUI32(&streamCapabilitiesSize, false)) {
		FATAL("Unable to read stream Capabilities Size");
		return false;
	}
	if (!raw.ReadFromFs(*_pSeekFile, streamCapabilitiesSize)) {
		FATAL("Unable to read raw stream Capabilities");
		return false;
	}
	if (!StreamCapabilities::Deserialize(raw, _streamCapabilities)) {
		FATAL("Unable to deserialize stream Capabilities. Please delete %s and %s files so they can be regenerated",
				STR(GetName() + "." MEDIA_TYPE_SEEK),
				STR(GetName() + "." MEDIA_TYPE_META));
		return false;
	}

	//4. compute offsets
	_seekBaseOffset = _pSeekFile->Cursor();
	_framesBaseOffset = _seekBaseOffset + 4;


	//5. Compute the optimal window size by reading the biggest frame size
	//from the seek file.
	if (!_pSeekFile->SeekTo(_pSeekFile->Size() - 8)) {
		FATAL("Unable to seek to %" PRIu64 " position", _pSeekFile->Cursor() - 8);
		return false;
	}
	uint64_t maxFrameSize = 0;
	if (!_pSeekFile->ReadUI64(&maxFrameSize, false)) {
		FATAL("Unable to read max frame size");
		return false;
	}
	if (!_pSeekFile->SeekBegin()) {
		FATAL("Unable to seek to beginning of the file");
		return false;
	}

	//3. Open the media file
	uint32_t windowSize = (uint32_t) maxFrameSize * 16;
	windowSize = windowSize < 65536 ? 65536 : windowSize;
	windowSize = (windowSize > (1024 * 1024)) ? (windowSize / 2) : windowSize;
	_pFile = GetFile(GetName(), windowSize);
	if (_pFile == NULL) {
		FATAL("Unable to initialize file");
		return false;
	}

	//4. Read the frames count from the file
	if (!_pSeekFile->SeekTo(_seekBaseOffset)) {
		FATAL("Unable to seek to _seekBaseOffset: %" PRIu64, _seekBaseOffset);
		return false;
	}
	if (!_pSeekFile->ReadUI32(&_totalFrames, false)) {
		FATAL("Unable to read the frames count");
		return false;
	}
	_timeToIndexOffset = _framesBaseOffset + _totalFrames * sizeof (MediaFrame);

	//5. Set the client side buffer length
	_clientSideBufferLength = clientSideBufferLength;

	//6. Create the timer
	if (hasTimer) {
		_pTimer = new InFileStreamTimer(this);
		_pTimer->EnqueueForTimeEvent(_clientSideBufferLength - _clientSideBufferLength / 3);
	}

	//7. Done
	return true;
}
#endif /* HAS_VOD_MANAGER */

bool BaseInFileStream::SignalPlay(double &absoluteTimestamp, double &length) {
	//0. fix absoluteTimestamp and length
	absoluteTimestamp = absoluteTimestamp < 0 ? 0 : absoluteTimestamp;
	_playLimit = length;
	//FINEST("absoluteTimestamp: %.2f; _playLimit: %.2f", absoluteTimestamp, _playLimit);
	//TODO: implement the limit playback

	//1. Seek to the correct point
	if (!InternalSeek(absoluteTimestamp)) {
		FATAL("Unable to seek to %.02f", absoluteTimestamp);
		return false;
	}

	//2. Put the stream in active mode
	_streamingState = FILE_STREAMING_STATE_PLAYING;

	//3. Start the feed reaction
	ReadyForSend();

	//4. Done
	return true;
}

bool BaseInFileStream::SignalPause() {
	//1. Is this already paused
	if (_streamingState != FILE_STREAMING_STATE_PLAYING)
		return true;

	//2. Put the stream in paused mode
	_streamingState = FILE_STREAMING_STATE_PAUSED;

	//3. Done
	return true;
}

bool BaseInFileStream::SignalResume() {
	//1. Is this already active
	if (_streamingState == FILE_STREAMING_STATE_PLAYING)
		return true;

	//2. Put the stream in active mode
	_streamingState = FILE_STREAMING_STATE_PLAYING;

	//3. Start the feed reaction
	ReadyForSend();

	//5. Done
	return true;
}

bool BaseInFileStream::SignalSeek(double &absoluteTimestamp) {
	//1. Seek to the correct point
	if (!InternalSeek(absoluteTimestamp)) {
		FATAL("Unable to seek to %.02f", absoluteTimestamp);
		return false;
	}

	//2. If the stream is active, re-initiate the feed reaction
	if (_streamingState == FILE_STREAMING_STATE_FINISHED) {
		_streamingState = FILE_STREAMING_STATE_PLAYING;
		ReadyForSend();
	}

	//3. Done
	return true;
}

bool BaseInFileStream::SignalStop() {
	//1. Is this already paused
	if (_streamingState != FILE_STREAMING_STATE_PLAYING)
		return true;

	//2. Put the stream in paused mode
	_streamingState = FILE_STREAMING_STATE_PAUSED;

	//3. Done
	return true;
}

void BaseInFileStream::ReadyForSend() {
	if (!Feed()) {
		FATAL("Feed failed");
		if (_pOutStreams != NULL)
			_pOutStreams->info->EnqueueForDelete();
	}
}

bool BaseInFileStream::InternalSeek(double &absoluteTimestamp) {
	//0. We have to send codecs again
	_audioVideoCodecsSent = false;

	//1. Switch to millisecond->FrameIndex table
	if (!_pSeekFile->SeekTo(_timeToIndexOffset)) {
		FATAL("Failed to seek to ms->FrameIndex table");
		return false;
	}

	//2. Read the sampling rate
	uint32_t samplingRate;
	if (!_pSeekFile->ReadUI32(&samplingRate, false)) {
		FATAL("Unable to read the frames per second");
		return false;
	}

	//3. compute the index in the time2frameindex
	uint32_t tableIndex = (uint32_t) (absoluteTimestamp / samplingRate);

	//4. Seek to that corresponding index
	_pSeekFile->SeekAhead(tableIndex * 4);

	//5. Read the frame index
	uint32_t frameIndex;
	if (!_pSeekFile->ReadUI32(&frameIndex, false)) {
		FATAL("Unable to read frame index");
		return false;
	}

	//7. Position the seek file to that particular frame
	if (!_pSeekFile->SeekTo(_framesBaseOffset + frameIndex * sizeof (MediaFrame))) {
		FATAL("Unablt to seek inside seek file");
		return false;
	}

	//8. Read the frame
	if (!_pSeekFile->ReadBuffer((uint8_t *) & _currentFrame, sizeof (MediaFrame))) {
		FATAL("Unable to read frame from seeking file");
		return false;
	}

	//9. update the stream counters
	_startFeedingTime = time(NULL);
	_totalSentTime = 0;
	_currentFrameIndex = frameIndex;
	_totalSentTimeBase = (uint32_t) (_currentFrame.absoluteTime / 1000);
	absoluteTimestamp = _currentFrame.absoluteTime;

	//10. Go back on the frame of interest
	if (!_pSeekFile->SeekTo(_framesBaseOffset + frameIndex * sizeof (MediaFrame))) {
		FATAL("Unablt to seek inside seek file");
		return false;
	}

	//11. Done
	return true;
}

bool BaseInFileStream::Feed() {
	//1. Are we in paused state?
	if (_streamingState != FILE_STREAMING_STATE_PLAYING)
		return true;

	//2. First, send audio and video codecs
	if (!_audioVideoCodecsSent) {
		if (!SendCodecs()) {
			FATAL("Unable to send audio codec");
			return false;
		}
	}

	//2. Determine if the client has enough data on the buffer and continue
	//or stay put
	uint32_t elapsedTime = (uint32_t) (time(NULL) - _startFeedingTime);
	if ((int32_t) _totalSentTime - (int32_t) elapsedTime >= (int32_t) _clientSideBufferLength) {
		return true;
	}

	//3. Test to see if we have sent the last frame
	if (_currentFrameIndex >= _totalFrames) {
		FINEST("Done streaming file");
		_pOutStreams->info->SignalStreamCompleted();
		_streamingState = FILE_STREAMING_STATE_FINISHED;
		return true;
	}

	//FINEST("_totalSentTime: %.2f; _playLimit: %.2f", (double) _totalSentTime, _playLimit);
	if (_playLimit >= 0) {
		if (_playLimit < (double) _totalSentTime) {
			FINEST("Done streaming file");
			_pOutStreams->info->SignalStreamCompleted();
			_streamingState = FILE_STREAMING_STATE_FINISHED;
			return true;
		}
	}

	//4. Read the current frame from the seeking file
	if (!_pSeekFile->SeekTo(_framesBaseOffset + _currentFrameIndex * sizeof (MediaFrame))) {
		FATAL("Unable to seek inside seek file");
		return false;
	}
	if (!_pSeekFile->ReadBuffer((uint8_t *) & _currentFrame, sizeof (_currentFrame))) {
		FATAL("Unable to read frame from seeking file");
		return false;
	}

	//5. Take care of metadata
	if (_currentFrame.type == MEDIAFRAME_TYPE_DATA) {
		_currentFrameIndex++;
		if (!FeedMetaData(_pFile, _currentFrame)) {
			FATAL("Unable to feed metadata");
			return false;
		}
		return Feed();
	}

	//6. get our hands on the correct buffer, depending on the frame type: audio or video
	IOBuffer &buffer = _currentFrame.type == MEDIAFRAME_TYPE_AUDIO ? _audioBuffer : _videoBuffer;

	//10. Discard the data
	buffer.IgnoreAll();

	//7. Build the frame
	if (!BuildFrame(_pFile, _currentFrame, buffer)) {
		FATAL("Unable to build the frame");
		return false;
	}

	//8. Compute the timestamp
	_totalSentTime = (uint32_t) (_currentFrame.absoluteTime / 1000) - _totalSentTimeBase;

	//11. Increment the frame index
	_currentFrameIndex++;

	//9. Do the feedeng
#ifdef HAS_VOD_MANAGER
	_servedBytes += GETAVAILABLEBYTESCOUNT(buffer);
#endif /* HAS_VOD_MANAGER */
	if (!_pOutStreams->info->FeedData(
			GETIBPOINTER(buffer), //pData
			GETAVAILABLEBYTESCOUNT(buffer), //dataLength
			0, //processedLength
			GETAVAILABLEBYTESCOUNT(buffer), //totalLength
			(uint32_t) _currentFrame.absoluteTime, //absoluteTimestamp
			_currentFrame.type == MEDIAFRAME_TYPE_AUDIO //isAudio
			)) {
		FATAL("Unable to feed audio data");
		return false;
	}

	//12. Done. We either feed again if frame length was 0
	//or just return true
	if (_currentFrame.length == 0) {
		return Feed();
	} else {
		return true;
	}
}

#ifdef HAS_MMAP

MmapFile* BaseInFileStream::GetFile(string filePath, uint32_t windowSize) {
	if (windowSize == 0)
		windowSize = 131072;
	MmapFile *pResult = NULL;
	pResult = new MmapFile();
	if (!pResult->Initialize(filePath, windowSize, false)) {
		delete pResult;
		return NULL;
	}
	return pResult;
}

void BaseInFileStream::ReleaseFile(MmapFile *pFile) {
	if (pFile == NULL)
		return;
	delete pFile;
}

#else

File* BaseInFileStream::GetFile(string filePath, uint32_t windowSize) {
	File *pResult = new File();
	if (!pResult->Initialize(filePath)) {
		delete pResult;
		return NULL;
	}
	return pResult;
}

void BaseInFileStream::ReleaseFile(File *pFile) {
	if (pFile == NULL)
		return;
	delete pFile;
}
#endif /* HAS_MMAP */

bool BaseInFileStream::SendCodecs() {
	//1. Read the first frame
	MediaFrame frame1;
	if (!_pSeekFile->SeekTo(_framesBaseOffset + 0 * sizeof (MediaFrame))) {
		FATAL("Unablt to seek inside seek file");
		return false;
	}
	if (!_pSeekFile->ReadBuffer((uint8_t *) & frame1, sizeof (MediaFrame))) {
		FATAL("Unable to read frame from seeking file");
		return false;
	}

	//2. Read the second frame
	MediaFrame frame2;
	if (!_pSeekFile->SeekTo(_framesBaseOffset + 1 * sizeof (MediaFrame))) {
		FATAL("Unablt to seek inside seek file");
		return false;
	}
	if (!_pSeekFile->ReadBuffer((uint8_t *) & frame2, sizeof (MediaFrame))) {
		FATAL("Unable to read frame from seeking file");
		return false;
	}

	//3. Read the current frame to pickup the timestamp from it
	MediaFrame currentFrame;
	if (!_pSeekFile->SeekTo(_framesBaseOffset + _currentFrameIndex * sizeof (MediaFrame))) {
		FATAL("Unablt to seek inside seek file");
		return false;
	}
	if (!_pSeekFile->ReadBuffer((uint8_t *) & currentFrame, sizeof (MediaFrame))) {
		FATAL("Unable to read frame from seeking file");
		return false;
	}

	//4. Is the first frame a codec setup?
	//If not, the second is not a codec setup for sure
	if (!frame1.isBinaryHeader) {
		_audioVideoCodecsSent = true;
		return true;
	}

	//5. Build the buffer for the first frame
	IOBuffer buffer;
	if (!BuildFrame(_pFile, frame1, buffer)) {
		FATAL("Unable to build the frame");
		return false;
	}

	//6. Do the feedeng with the first frame
#ifdef HAS_VOD_MANAGER
	_servedBytes += GETAVAILABLEBYTESCOUNT(buffer);
#endif /* HAS_VOD_MANAGER */
	if (!_pOutStreams->info->FeedData(
			GETIBPOINTER(buffer), //pData
			GETAVAILABLEBYTESCOUNT(buffer), //dataLength
			0, //processedLength
			GETAVAILABLEBYTESCOUNT(buffer), //totalLength
			currentFrame.absoluteTime, //absoluteTimestamp
			frame1.type == MEDIAFRAME_TYPE_AUDIO //isAudio
			)) {
		FATAL("Unable to feed audio data");
		return false;
	}

	//7. Is the second frame a codec setup?
	if (!frame2.isBinaryHeader) {
		_audioVideoCodecsSent = true;
		return true;
	}

	//8. Build the buffer for the second frame
	buffer.IgnoreAll();
	if (!BuildFrame(_pFile, frame2, buffer)) {
		FATAL("Unable to build the frame");
		return false;
	}

	//9. Do the feedeng with the second frame
#ifdef HAS_VOD_MANAGER
	_servedBytes += GETAVAILABLEBYTESCOUNT(buffer);
#endif /* HAS_VOD_MANAGER */
	if (!_pOutStreams->info->FeedData(
			GETIBPOINTER(buffer), //pData
			GETAVAILABLEBYTESCOUNT(buffer), //dataLength
			0, //processedLength
			GETAVAILABLEBYTESCOUNT(buffer), //totalLength
			currentFrame.absoluteTime, //absoluteTimestamp
			frame2.type == MEDIAFRAME_TYPE_AUDIO //isAudio
			)) {
		FATAL("Unable to feed audio data");
		return false;
	}

	//10. Done
	_audioVideoCodecsSent = true;
	return true;
}

#ifdef HAS_VOD_MANAGER

void BaseInFileStream::UpdateOpenCountInfo() {
	Variant info;
	Variant::DeserializeFromXmlFile(_infoFilePath, info);
	uint64_t openCount = 0;
	//uint64_t totalAccessCount = 0;
	if (info.HasKeyChain(_V_NUMERIC, false, 1, "openCount")) {
		openCount = (uint64_t) info.GetValue("openCount", false);
	}
	info["openCount"] = (uint64_t) (openCount + 1);
	info["filePaths"] = _filePaths;
	info.SerializeToXmlFile(_infoFilePath);
}

void BaseInFileStream::UpdateServedBytesInfo() {
	if (_mediaFileSize == 0)
		return;
	Variant info;
	Variant::DeserializeFromXmlFile(_infoFilePath, info);
	uint64_t totalServedBytes = 0;
	//uint64_t totalAccessCount = 0;
	if (info.HasKeyChain(_V_NUMERIC, false, 1, "totalServedBytes")) {
		totalServedBytes = (uint64_t) info.GetValue("totalServedBytes", false);
	}
	info["totalServedBytes"] = (uint64_t) (_servedBytes + totalServedBytes);
	info["fileSize"] = _mediaFileSize;
	info["serveRatio"] = (double) ((double) (_servedBytes + totalServedBytes) / (double) _mediaFileSize);
	info.SerializeToXmlFile(_infoFilePath);
}
#endif /* HAS_VOD_MANAGER */
