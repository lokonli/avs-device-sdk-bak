/*
 * NSKeyWordDetector.cpp
 *
 * Copyright 2017 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#include <memory>
#include <sstream>

#include <AVSCommon/Utils/Logger/Logger.h>
#include <AVSCommon/Utils/Memory/Memory.h>

#include "NS/NSKeyWordDetector.h"

/*
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#define PORT 8095
*/
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>


#include <sys/types.h>  // mkfifo
#include <sys/stat.h>   // mkfifo
#include <stdio.h>
#include <iostream>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

namespace alexaClientSDK {
namespace kwd {

using namespace avsCommon;
using namespace avsCommon::avs;
using namespace avsCommon::sdkInterfaces;
using namespace avsCommon::utils;

static const std::string TAG("NSKeyWordDetector");

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsCommon::utils::logger::LogEntry(TAG, event)

/// The number of hertz per kilohertz.
static const size_t HERTZ_PER_KILOHERTZ = 1000;

/// The timeout to use for read calls to the SharedDataStream.
const std::chrono::milliseconds TIMEOUT_FOR_READ_CALLS = std::chrono::milliseconds(1000);

/// The delimiter for Kitt.ai engine constructor parameters
static const std::string KITT_DELIMITER = ",";

/// The Kitt.ai compatible audio encoding of LPCM.
static const avsCommon::utils::AudioFormat::Encoding KITT_AI_COMPATIBLE_ENCODING =
    avsCommon::utils::AudioFormat::Encoding::LPCM;

/// The Kitt.ai compatible endianness which is little endian.
static const avsCommon::utils::AudioFormat::Endianness KITT_AI_COMPATIBLE_ENDIANNESS =
    avsCommon::utils::AudioFormat::Endianness::LITTLE;

/// Kitt.ai returns -2 if silence is detected.
static const int KITT_AI_SILENCE_DETECTION_RESULT = -2;

/// Kitt.ai returns -1 if an error occurred.
static const int KITT_AI_ERROR_DETECTION_RESULT = -1;

/// Kitt.ai returns 0 if no keyword was detected but audio has been heard.
static const int KITT_AI_NO_DETECTION_RESULT = 0;

std::unique_ptr<NSKeyWordDetector> NSKeyWordDetector::create(
     std::shared_ptr<defaultClient::DefaultClient> client,
    std::shared_ptr<AudioInputStream> stream,
    AudioFormat audioFormat,
    std::unordered_set<std::shared_ptr<KeyWordObserverInterface>> keyWordObservers,
    std::unordered_set<std::shared_ptr<KeyWordDetectorStateObserverInterface>> keyWordDetectorStateObservers,
    std::chrono::milliseconds msToPushPerIteration) {
    if (!stream) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullStream"));
        return nullptr;
    }
    // TODO: ACSDK-249 - Investigate cpu usage of converting bytes between endianness and if it's not too much, do it.
    if (isByteswappingRequired(audioFormat)) {
        ACSDK_ERROR(LX("createFailed").d("reason", "endianMismatch"));
        return nullptr;
    }
    std::unique_ptr<NSKeyWordDetector> detector(new NSKeyWordDetector(
	client,        
	stream,
        audioFormat,
        keyWordObservers,
        keyWordDetectorStateObservers,
        msToPushPerIteration));
    if (!detector->init(audioFormat)) {
        ACSDK_ERROR(LX("createFailed").d("reason", "initDetectorFailed"));
        return nullptr;
    }
    return detector;
}

NSKeyWordDetector::~NSKeyWordDetector() {
    m_isShuttingDown = true;
    if (m_detectionThread.joinable()) {
        m_detectionThread.join();
    }
}

NSKeyWordDetector::NSKeyWordDetector(
       std::shared_ptr<defaultClient::DefaultClient> client, 
   std::shared_ptr<AudioInputStream> stream,
    avsCommon::utils::AudioFormat audioFormat,
    std::unordered_set<std::shared_ptr<KeyWordObserverInterface>> keyWordObservers,
    std::unordered_set<std::shared_ptr<KeyWordDetectorStateObserverInterface>> keyWordDetectorStateObservers,
    std::chrono::milliseconds msToPushPerIteration) :
/*        AbstractKeywordDetector(keyWordObservers, keyWordDetectorStateObservers),
        m_stream{stream},
        m_maxSamplesPerPush{(audioFormat.sampleRateHz / HERTZ_PER_KILOHERTZ) * msToPushPerIteration.count()} {
    std::stringstream sensitivities;
    std::stringstream modelPaths;*/
        AbstractKeywordDetector(keyWordObservers, keyWordDetectorStateObservers),
	m_client{client},
        m_stream{stream},
 //       m_session{nullptr},
        m_maxSamplesPerPush((audioFormat.sampleRateHz / HERTZ_PER_KILOHERTZ) * msToPushPerIteration.count()) {
}

bool NSKeyWordDetector::init(avsCommon::utils::AudioFormat audioFormat) {
    if (!isAudioFormatCompatibleWithNS(audioFormat)) {
        return false;
    }
/* lok: volgens mij heb ik geen reader nodig. Even laten staan ... */
    m_streamReader = m_stream->createReader(AudioInputStream::Reader::Policy::BLOCKING);
    
   if (!m_streamReader) {
        ACSDK_ERROR(LX("initFailed").d("reason", "createStreamReaderFailed"));
        return false;
    }

// lok: copied from MicWrapper
    m_writer = m_stream->createWriter(AudioInputStream::Writer::Policy::NONBLOCKABLE);
    if (!m_writer) {
         ACSDK_ERROR(LX("initFailed").d("reason", "createStreamWriterFailed"));
        return false;
    }
  

    m_fifo = mkfifo("/tmp/alexafifo", S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
 
   if(m_fifo) {
	   ACSDK_ERROR(LX("initFailed").d("reason", "fifo non zero"));
	}




  
   ACSDK_ERROR(LX("initFailed").d("reason", "fifo created"));
 
    m_isShuttingDown = false;
    m_detectionThread = std::thread(&NSKeyWordDetector::detectionLoop, this);
    return true;
}

bool NSKeyWordDetector::isAudioFormatCompatibleWithNS(avsCommon::utils::AudioFormat audioFormat) {

    return true;
}

void NSKeyWordDetector::detectionLoop() {
    notifyKeyWordDetectorStateObservers(KeyWordDetectorStateObserverInterface::KeyWordDetectorState::ACTIVE);
//    int16_t audioDataToPush[m_maxSamplesPerPush];
//    ssize_t wordsRead;
    char temp[1024];
	   ACSDK_ERROR(LX("readloop").d("reason", "starting loop"));
       bool didErrorOccur=false;

    if ((m_fifo = open("/tmp/alexafifo", O_RDONLY)) < 0) {
 	   ACSDK_ERROR(LX("initFailed").d("reason", "Not open for reading"));
        didErrorOccur= true;
    }

	   ACSDK_INFO(LX("readloop").d("reason", "Fifo opened"));
//Data must be available, so we trigger all observers
    int detectionResult = 1;
    while (!m_isShuttingDown) {
	int num;
		

//               if ((num = read(m_fifo, temp, sizeof(temp))) < 0) {
               if ((num = read(m_fifo, temp, sizeof(temp))) < 0) {
 	       ACSDK_ERROR(LX("readloop").d("reason", "reading failed"));
		didErrorOccur = true;
     	}
 
	while(num==0) {

		//num==0 while fifo was opened
		//that means EOF was reached
	 	   ACSDK_INFO(LX("detectionloop").d("reason", "EOF reached. Waiting"));
         //          notifyKeyWordDetectorStateObservers(
          //              KeyWordDetectorStateObserverInterface::KeyWordDetectorState::STREAM_CLOSED);
		// m_client->stopForegroundActivity(); //to bring back to idle
		// we will write some additional 0 to trigger speech.
		char sample[2];
		sample[0]=0; sample[1]=0;
		for(int i=0; i<4096;i++) {
					m_writer->write(sample, 1);  //write something
		}

 
	   if ((m_fifo = open("/tmp/alexafifo", O_RDONLY)) < 0) {
	 	   ACSDK_ERROR(LX("initFailed").d("reason", "Not open for reading"));
		didErrorOccur= true;
	    }

               if ((num = read(m_fifo, temp, sizeof(temp))) < 0) {
 	   		ACSDK_ERROR(LX("readloop").d("reason", "reading failed"));
			didErrorOccur = true;
     		}
		else {
			detectionResult = 1;
		}
 
	}



/*
        wordsRead = readFromStream(
            m_streamReader, m_stream, audioDataToPush, m_maxSamplesPerPush, TIMEOUT_FOR_READ_CALLS, &didErrorOccur);
*/

            // Words were successfully read.
	 	   ACSDK_INFO(LX("readloop").d("reason", "reading success"));
	if (num>0) {	
		int pos = m_writer->tell();
		m_writer->write(temp, num>>1);  //write something
	 	if (detectionResult) {
		 	   ACSDK_INFO(LX("readloop").d("reason", "trigger KWDStateObserver"));
		           notifyKeyWordDetectorStateObservers(KeyWordDetectorStateObserverInterface::KeyWordDetectorState::ACTIVE);
			detectionResult = 0;
 //lok */
			 notifyKeyWordObservers(
		                m_stream,
//		                m_detectionResultsToKeyWords[detectionResult],
				"",
		                KeyWordObserverInterface::UNSPECIFIED_INDEX,
//		                m_streamReader->tell());		//ik weet niet wat dit doet
		                pos);		//geeft de eindpositie aan.
//				8);					//hiermee gaat de buffer vol na een aantal keer ...
	 	 }
	}
	if (didErrorOccur) {
	 	   ACSDK_ERROR(LX("readloop").d("reason", "There was an error somewhere"));
			didErrorOccur = false;
		}
		
  //          int detectionResult = m_kittAiEngine->RunDetection(audioDataToPush, wordsRead);
  	}  //while !shutting down
    m_streamReader->close();
	//todo: close fifo, m_writer
}

}  // namespace kwd
}  // namespace alexaClientSDK
