/*
 * Copyright (c) 2010-2016 Stephane Poirier
 *
 * stephane.poirier@oifii.org
 *
 * Stephane Poirier
 * 3532 rue Ste-Famille, #3
 * Montreal, QC, H2X 2L1
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

////////////////////////////////////////////////////////////////
//nakedsoftware.org, spi@oifii.org or stephane.poirier@oifii.org
//
//
//2013april02, creation for recording a stereo wav file using portaudio.
//			   initially derived from paex_record_file and modified so
//			   the wav file is recording in wav file format instead of
//			   the raw format that was used in paex_record_file.c
//			   also, support for asio and device selection from arguments.
//
//2014may04, added a pause/unpause recording feature attached to key 'P'
//
//nakedsoftware.org, spi@oifii.org or stephane.poirier@oifii.org
////////////////////////////////////////////////////////////////
 
#include <stdio.h>
#include <stdlib.h>
#include "portaudio.h"
#include "pa_asio.h"
#include "pa_ringbuffer.h"
#include "pa_util.h"

#include <sndfile.hh>
#include <assert.h>
#include <map>
#include <string>
using namespace std;

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#endif

#include <conio.h> //for _kbhit()

 
/* #define SAMPLE_RATE  (17932) // Test failure to open with this value. */
#define FILE_NAME       "audio_data.raw"
#define SAMPLE_RATE  (44100)
#define FRAMES_PER_BUFFER (512)
#define NUM_SECONDS     (60)
#define NUM_CHANNELS    (2)
#define NUM_WRITES_PER_BUFFER   (4)
/* #define DITHER_FLAG     (paDitherOff) */
#define DITHER_FLAG     (0) /**/
 


/* Select sample format. */
#if 1
#define PA_SAMPLE_TYPE  paFloat32
typedef float SAMPLE;
#define SAMPLE_SILENCE  (0.0f)
#define PRINTF_S_FORMAT "%.8f"
#elif 1
#define PA_SAMPLE_TYPE  paInt16
typedef short SAMPLE;
#define SAMPLE_SILENCE  (0)
#define PRINTF_S_FORMAT "%d"
#elif 0
#define PA_SAMPLE_TYPE  paInt8
typedef char SAMPLE;
#define SAMPLE_SILENCE  (0)
#define PRINTF_S_FORMAT "%d"
#else
#define PA_SAMPLE_TYPE  paUInt8
typedef unsigned char SAMPLE;
#define SAMPLE_SILENCE  (128)
#define PRINTF_S_FORMAT "%d"
#endif
 

//The event signaled when the app should be terminated.
HANDLE g_hTerminateEvent = NULL;
//Handles events that would normally terminate a console application. 
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType);

int Terminate();
string global_filename;
map<string,int> global_devicemap;
PaStreamParameters global_inputParameters;
PaError global_err;
string global_audiodevicename;
int global_inputAudioChannelSelectors[2];
PaAsioStreamInfo global_asioInputInfo;

bool global_pauserecording=false;

bool SelectAudioDevice()
{
	const PaDeviceInfo* deviceInfo;
    int numDevices = Pa_GetDeviceCount();
    for( int i=0; i<numDevices; i++ )
    {
        deviceInfo = Pa_GetDeviceInfo( i );
		string devicenamestring = deviceInfo->name;
		global_devicemap.insert(pair<string,int>(devicenamestring,i));
	}

	int deviceid = Pa_GetDefaultInputDevice(); // default input device 
	map<string,int>::iterator it;
	it = global_devicemap.find(global_audiodevicename);
	if(it!=global_devicemap.end())
	{
		deviceid = (*it).second;
		printf("%s maps to %d\n", global_audiodevicename.c_str(), deviceid);
		deviceInfo = Pa_GetDeviceInfo(deviceid);
		//assert(inputAudioChannelSelectors[0]<deviceInfo->maxInputChannels);
		//assert(inputAudioChannelSelectors[1]<deviceInfo->maxInputChannels);
	}
	else
	{
		for(it=global_devicemap.begin(); it!=global_devicemap.end(); it++)
		{
			printf("%s maps to %d\n", (*it).first.c_str(), (*it).second);
		}
		//Pa_Terminate();
		//return -1;
		printf("error, audio device not found, will use default\n");
		deviceid = Pa_GetDefaultInputDevice();
	}


	global_inputParameters.device = deviceid; 
	if (global_inputParameters.device == paNoDevice) 
	{
		fprintf(stderr,"Error: No default input device.\n");
		//goto error;
		Pa_Terminate();
		fprintf( stderr, "An error occured while using the portaudio stream\n" );
		fprintf( stderr, "Error number: %d\n", global_err );
		fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( global_err ) );
		return Terminate();
	}
	global_inputParameters.channelCount = 2;
	global_inputParameters.sampleFormat =  PA_SAMPLE_TYPE;
	global_inputParameters.suggestedLatency = Pa_GetDeviceInfo( global_inputParameters.device )->defaultLowOutputLatency;
	//inputParameters.hostApiSpecificStreamInfo = NULL;

	//Use an ASIO specific structure. WARNING - this is not portable. 
	//PaAsioStreamInfo asioInputInfo;
	global_asioInputInfo.size = sizeof(PaAsioStreamInfo);
	global_asioInputInfo.hostApiType = paASIO;
	global_asioInputInfo.version = 1;
	global_asioInputInfo.flags = paAsioUseChannelSelectors;
	global_asioInputInfo.channelSelectors = global_inputAudioChannelSelectors;
	if(deviceid==Pa_GetDefaultInputDevice())
	{
		global_inputParameters.hostApiSpecificStreamInfo = NULL;
	}
	else if(Pa_GetHostApiInfo(Pa_GetDeviceInfo(deviceid)->hostApi)->type == paASIO) 
	{
		global_inputParameters.hostApiSpecificStreamInfo = &global_asioInputInfo;
	}
	else if(Pa_GetHostApiInfo(Pa_GetDeviceInfo(deviceid)->hostApi)->type == paWDMKS) 
	{
		global_inputParameters.hostApiSpecificStreamInfo = NULL;
	}
	else
	{
		//assert(false);
		global_inputParameters.hostApiSpecificStreamInfo = NULL;
	}
	return true;
}


 
typedef struct
{
    unsigned            frameIndex;
    int                 threadSyncFlag;
    SAMPLE             *ringBufferData;
    PaUtilRingBuffer    ringBuffer;
    FILE               *file;
    void               *threadHandle;
}
 
paTestData;
 
bool AppendWavFile(const char* filename, const void* pVoid, long sizeelementinbytes, long count)
{
	assert(filename);
	assert(sizeelementinbytes==4);
    const int format=SF_FORMAT_WAV | SF_FORMAT_PCM_16;  
	//const int format=SF_FORMAT_WAV | SF_FORMAT_FLOAT;  
    //const int format=SF_FORMAT_WAV | SF_FORMAT_PCM_24;  
    //const int format=SF_FORMAT_WAV | SF_FORMAT_PCM_32;  
	//SndfileHandle outfile(filename, SFM_WRITE, format, numChannels, SampleRate); 

	SndfileHandle outfile(filename, SFM_RDWR, format, NUM_CHANNELS, SAMPLE_RATE); 
	//SndfileHandle outfile(filename, SFM_RDWR, format, 1, SAMPLE_RATE); 
	outfile.seek(outfile.frames(), SEEK_SET);
	/*
	if(frameIndex==0)
	{
		outfile.write(pSamples, numSamples);  
	}
	else
	{
		assert(frameIndex<=totalFrames);
		outfile.write(pSamples, frameIndex*NUM_CHANNELS); 
	}
	*/
	outfile.write((const float*)pVoid, count); 
	return true;
}

// This routine is run in a separate thread to write data from the ring buffer into a wav file (during Recording)
static int threadFunctionWriteToWavFile(void* ptr)
{
    paTestData* pData = (paTestData*)ptr;
 
    // Mark thread started  
    pData->threadSyncFlag = 0;
 
    while (1)
    {
        ring_buffer_size_t elementsInBuffer = PaUtil_GetRingBufferReadAvailable(&pData->ringBuffer);
        if ( (elementsInBuffer >= pData->ringBuffer.bufferSize / NUM_WRITES_PER_BUFFER) ||
             pData->threadSyncFlag )
        {
            void* ptr[2] = {0};
            ring_buffer_size_t sizes[2] = {0};
 
            /* By using PaUtil_GetRingBufferReadRegions, we can read directly from the ring buffer */
            ring_buffer_size_t elementsRead = PaUtil_GetRingBufferReadRegions(&pData->ringBuffer, elementsInBuffer, ptr + 0, sizes + 0, ptr + 1, sizes + 1);
            if (elementsRead > 0)
            {
                int i;
                for (i = 0; i < 2 && ptr[i] != NULL; ++i)
                {
                    //fwrite(ptr[i], pData->ringBuffer.elementSizeBytes, sizes[i], pData->file);
					AppendWavFile(global_filename.c_str(), ptr[i], pData->ringBuffer.elementSizeBytes, sizes[i]);
                }
                PaUtil_AdvanceRingBufferReadIndex(&pData->ringBuffer, elementsRead);
            }
 
            if (pData->threadSyncFlag)
            {
                break;
            }
        }
 
        /* Sleep a little while... */
        Pa_Sleep(20);
 
    }
 
    pData->threadSyncFlag = 0;
    return 0;
}

/* This routine is run in a separate thread to write data from the ring buffer into a file (during Recording) */ 
static int threadFunctionWriteToRawFile(void* ptr)
{
    paTestData* pData = (paTestData*)ptr;
 
    /* Mark thread started */ 
    pData->threadSyncFlag = 0;
 
    while (1)
    {
        ring_buffer_size_t elementsInBuffer = PaUtil_GetRingBufferReadAvailable(&pData->ringBuffer);
        if ( (elementsInBuffer >= pData->ringBuffer.bufferSize / NUM_WRITES_PER_BUFFER) ||
             pData->threadSyncFlag )
        {
            void* ptr[2] = {0};
            ring_buffer_size_t sizes[2] = {0};
 
            /* By using PaUtil_GetRingBufferReadRegions, we can read directly from the ring buffer */
            ring_buffer_size_t elementsRead = PaUtil_GetRingBufferReadRegions(&pData->ringBuffer, elementsInBuffer, ptr + 0, sizes + 0, ptr + 1, sizes + 1);
            if (elementsRead > 0)
            {
                int i;
                for (i = 0; i < 2 && ptr[i] != NULL; ++i)
                {
                    fwrite(ptr[i], pData->ringBuffer.elementSizeBytes, sizes[i], pData->file);
                }
                PaUtil_AdvanceRingBufferReadIndex(&pData->ringBuffer, elementsRead);
            }
 
            if (pData->threadSyncFlag)
            {
                break;
            }
        }
 
        /* Sleep a little while... */
        Pa_Sleep(20);
 
    }
 
    pData->threadSyncFlag = 0;
    return 0;
}
 

 
/* This routine is run in a separate thread to read data from file into the ring buffer (during Playback). When the file
   has reached EOF, a flag is set so that the play PA callback can return paComplete */ 
static int threadFunctionReadFromRawFile(void* ptr)
{
    paTestData* pData = (paTestData*)ptr;

    while (1)
    {
        ring_buffer_size_t elementsInBuffer = PaUtil_GetRingBufferWriteAvailable(&pData->ringBuffer);
 
        if (elementsInBuffer >= pData->ringBuffer.bufferSize / NUM_WRITES_PER_BUFFER)
        {
            void* ptr[2] = {0};
            ring_buffer_size_t sizes[2] = {0};
 
            /* By using PaUtil_GetRingBufferWriteRegions, we can write directly into the ring buffer */
            PaUtil_GetRingBufferWriteRegions(&pData->ringBuffer, elementsInBuffer, ptr + 0, sizes + 0, ptr + 1, sizes + 1);
 
            if (!feof(pData->file))
            {
                ring_buffer_size_t itemsReadFromFile = 0;
                int i;
                for (i = 0; i < 2 && ptr[i] != NULL; ++i)
                {
                    itemsReadFromFile += (ring_buffer_size_t)fread(ptr[i], pData->ringBuffer.elementSizeBytes, sizes[i], pData->file);
                }
                PaUtil_AdvanceRingBufferWriteIndex(&pData->ringBuffer, itemsReadFromFile);
 
                /* Mark thread started here, that way we "prime" the ring buffer before playback */
                pData->threadSyncFlag = 0;
            }
            else
            {
                /* No more data to read */
                pData->threadSyncFlag = 1;
                break;
            }
        }
 
        /* Sleep a little while... */
        Pa_Sleep(20);
    }
    return 0;
}
 

 
typedef int (*ThreadFunctionType)(void*);

/* Start up a new thread in the given function, at the moment only Windows, but should be very easy to extend
 
   to posix type OSs (Linux/Mac) */
 
static PaError startThread( paTestData* pData, ThreadFunctionType fn ) 
{
#ifdef _WIN32
    typedef unsigned (__stdcall* WinThreadFunctionType)(void*);
    pData->threadHandle = (void*)_beginthreadex(NULL, 0, (WinThreadFunctionType)fn, pData, CREATE_SUSPENDED, NULL);
    if (pData->threadHandle == NULL) return paUnanticipatedHostError;
 
    /* Set file thread to a little higher prio than normal */
    SetThreadPriority(pData->threadHandle, THREAD_PRIORITY_ABOVE_NORMAL);
 
    /* Start it up */
    pData->threadSyncFlag = 1;
    ResumeThread(pData->threadHandle);
#endif
 
    /* Wait for thread to startup */
    while (pData->threadSyncFlag) {
        Pa_Sleep(10);
    }
 
    return paNoError;
}
 

 
static int stopThread( paTestData* pData )
{
    pData->threadSyncFlag = 1;
    /* Wait for thread to stop */
    while (pData->threadSyncFlag) {
        Pa_Sleep(10);
    }
 
#ifdef _WIN32
    CloseHandle(pData->threadHandle);
    pData->threadHandle = 0;
#endif

    return paNoError;
}
 

 

 
/* This routine will be called by the PortAudio engine when audio is needed.
** It may be called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int recordCallback( const void *inputBuffer, void *outputBuffer,
                           unsigned long framesPerBuffer,
                           const PaStreamCallbackTimeInfo* timeInfo,
                           PaStreamCallbackFlags statusFlags,
                           void *userData )
{
	if(global_pauserecording) return paContinue;

    paTestData *data = (paTestData*)userData;
    ring_buffer_size_t elementsWriteable = PaUtil_GetRingBufferWriteAvailable(&data->ringBuffer);
    ring_buffer_size_t elementsToWrite = min(elementsWriteable, (ring_buffer_size_t)(framesPerBuffer * NUM_CHANNELS));
    const SAMPLE *rptr = (const SAMPLE*)inputBuffer;
 
    (void) outputBuffer; /* Prevent unused variable warnings. */
    (void) timeInfo;
    (void) statusFlags;
    (void) userData;
 
    data->frameIndex += PaUtil_WriteRingBuffer(&data->ringBuffer, rptr, elementsToWrite);
 
    return paContinue;
}
 

 
/* This routine will be called by the PortAudio engine when audio is needed.
** It may be called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int playCallback( const void *inputBuffer, void *outputBuffer, 
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void *userData )
{
    paTestData *data = (paTestData*)userData;
    ring_buffer_size_t elementsToPlay = PaUtil_GetRingBufferReadAvailable(&data->ringBuffer);
    ring_buffer_size_t elementsToRead = min(elementsToPlay, (ring_buffer_size_t)(framesPerBuffer * NUM_CHANNELS));
    SAMPLE* wptr = (SAMPLE*)outputBuffer;
 
    (void) inputBuffer; /* Prevent unused variable warnings. */
    (void) timeInfo;
    (void) statusFlags;
    (void) userData;
 
    data->frameIndex += PaUtil_ReadRingBuffer(&data->ringBuffer, wptr, elementsToRead);
    return data->threadSyncFlag ? paComplete : paContinue;
}
 

 
static unsigned NextPowerOf2(unsigned val)
{
    val--;
    val = (val >> 1) | val;
    val = (val >> 2) | val;
    val = (val >> 4) | val;
    val = (val >> 8) | val;
    val = (val >> 16) | val;
    return ++val;
}
 

//migrated data out of the main scope so Terminate() can see it
paTestData          data = {0};
PaStream*           stream;
PaError             err = paNoError;

 
/*******************************************************************/
int main(int argc, char *argv[]);
int main(int argc, char *argv[])
{
	int nShowCmd = false;
	ShellExecuteA(NULL, "open", "begin.bat", "", NULL, nShowCmd);

	///////////////////
	//read in arguments
	///////////////////
	global_filename = "testrecording.wav"; //usage: spirecord testrecording.wav 10 "E-MU ASIO" 0 1
	float fSecondsRecord = NUM_SECONDS; 
	if(argc>1)
	{
		//first argument is the filename
		global_filename = argv[1];
	}
	if(argc>2)
	{
		//second argument is the time it will play
		fSecondsRecord = atof(argv[2]);
	}
	//use audio_spi\spidevicesselect.exe to find the name of your devices, only exact name will be matched (name as detected by spidevicesselect.exe)  
	global_audiodevicename="E-MU ASIO"; //"Wave (2- E-MU E-DSP Audio Proce"
	//string audiodevicename="Wave (2- E-MU E-DSP Audio Proce"; //"E-MU ASIO"
	if(argc>3)
	{
		global_audiodevicename = argv[3]; //for spi, device name could be "E-MU ASIO", "Speakers (2- E-MU E-DSP Audio Processor (WDM))", etc.
	}
	global_inputAudioChannelSelectors[0] = 0; // on emu patchmix ASIO device channel 1 (left)
	global_inputAudioChannelSelectors[1] = 1; // on emu patchmix ASIO device channel 2 (right)
	//global_inputAudioChannelSelectors[0] = 2; // on emu patchmix ASIO device channel 3 (left)
	//global_inputAudioChannelSelectors[1] = 3; // on emu patchmix ASIO device channel 4 (right)
	//global_inputAudioChannelSelectors[0] = 8; // on emu patchmix ASIO device channel 9 (left)
	//global_inputAudioChannelSelectors[1] = 9; // on emu patchmix ASIO device channel 10 (right)
	//global_inputAudioChannelSelectors[0] = 10; // on emu patchmix ASIO device channel 11 (left)
	//global_inputAudioChannelSelectors[1] = 11; // on emu patchmix ASIO device channel 12 (right)
	if(argc>4)
	{
		global_inputAudioChannelSelectors[0]=atoi(argv[4]); //0 for first asio channel (left) or 2, 4, 6, etc.
	}
	if(argc>5)
	{
		global_inputAudioChannelSelectors[1]=atoi(argv[5]); //1 for second asio channel (right) or 3, 5, 7, etc.
	}

    //Auto-reset, initially non-signaled event 
    g_hTerminateEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    //Add the break handler
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    //PaStreamParameters  inputParameters;
    PaStreamParameters  outputParameters;
    //PaStream*           stream;
    //PaError             err = paNoError;
    //paTestData          data = {0};
    //unsigned            delayCntr;
    float            delayCntr;
    unsigned            numSamples;
    unsigned            numBytes;
 
    printf("patest_record.c\n"); fflush(stdout);
 
    // We set the ring buffer size to about 500 ms
    numSamples = NextPowerOf2((unsigned)(SAMPLE_RATE * 0.5 * NUM_CHANNELS));
    numBytes = numSamples * sizeof(SAMPLE);
    data.ringBufferData = (SAMPLE *) PaUtil_AllocateMemory( numBytes );
    if( data.ringBufferData == NULL )
    {
        printf("Could not allocate ring buffer data.\n");
        goto done;
    }
 
    if (PaUtil_InitializeRingBuffer(&data.ringBuffer, sizeof(SAMPLE), numSamples, data.ringBufferData) < 0)
    {
        printf("Failed to initialize ring buffer. Size is not power of 2 ??\n");
        goto done;
    }
 
    err = Pa_Initialize();
    if( err != paNoError ) goto done;
 
 
	if(0)
	{
		global_inputParameters.device = Pa_GetDefaultInputDevice(); //default input device 
		if (global_inputParameters.device == paNoDevice) 
		{
			fprintf(stderr,"Error: No default input device.\n");
			goto done;
		}
		global_inputParameters.channelCount = 2;                    // stereo input
		global_inputParameters.sampleFormat = PA_SAMPLE_TYPE;
		global_inputParameters.suggestedLatency = Pa_GetDeviceInfo( global_inputParameters.device )->defaultLowInputLatency;
		global_inputParameters.hostApiSpecificStreamInfo = NULL;
	}
	else
	{
		////////////////////////
		//audio device selection
		////////////////////////
		SelectAudioDevice();
	}


    /* Record some audio. -------------------------------------------- */
    err = Pa_OpenStream(
              &stream,
              &global_inputParameters,
              NULL,                  /* &outputParameters, */
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,
              paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              recordCallback,
              &data );
 
    if( err != paNoError ) goto done;
 
	if(0)
	{
		// Open the raw audio 'cache' file...
		data.file = fopen(FILE_NAME, "wb");
		if (data.file == 0) goto done;
	}
	else
	{
		// Open the wav audio 'cache' file...
		data.file = fopen(global_filename.c_str(), "wb");
		if (data.file == 0) goto done;
	}

    /* Start the file writing thread */
    if(0)
	{
		err = startThread(&data, threadFunctionWriteToRawFile);
	}
	else
	{
		err = startThread(&data, threadFunctionWriteToWavFile);
	}
	if( err != paNoError ) goto done;
 
    err = Pa_StartStream( stream );
    if( err != paNoError ) goto done;
    printf("\n=== Now recording to '" FILE_NAME "' for %f seconds!! Press P to pause/unpause recording. ===\n", fSecondsRecord); fflush(stdout);
 
    /* Note that the RECORDING part is limited with TIME, not size of the file and/or buffer, so you can
       increase NUM_SECONDS until you run out of disk */
    delayCntr = 0;
    //while( delayCntr++ < fSecondsRecord )
    while( delayCntr < fSecondsRecord )
    {
        //printf("index = %d\n", data.frameIndex ); fflush(stdout);
        printf("rec time = %f\n", delayCntr ); fflush(stdout);
		if(_kbhit() && _getch()=='p')
		{
			if(global_pauserecording==false)
			{
				global_pauserecording=true;
				printf("pause pressed\n"); fflush(stdout);
			}
			else
			{
				global_pauserecording=false;
				printf("unpause pressed\n"); fflush(stdout);
			}
		}
        Pa_Sleep(1000);
		if(!global_pauserecording) delayCntr++;
    }
    if( err < 0 ) goto done;
 
	/*
    err = Pa_CloseStream( stream );
    if( err != paNoError ) goto done;
 
    // Stop the thread 
    err = stopThread(&data);
    if( err != paNoError ) goto done;
 
    // Close file 
    fclose(data.file);
    data.file = 0;
	*/

	/*
    // Playback recorded data.  -------------------------------------------- 
    data.frameIndex = 0;
 
    outputParameters.device = Pa_GetDefaultOutputDevice(); // default output device 
    if (outputParameters.device == paNoDevice) {
        fprintf(stderr,"Error: No default output device.\n");
        goto done;
    }
    outputParameters.channelCount = 2;                     // stereo output 
    outputParameters.sampleFormat =  PA_SAMPLE_TYPE;
    outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;

    printf("\n=== Now playing back from file '" FILE_NAME "' until end-of-file is reached ===\n"); fflush(stdout);
 
    err = Pa_OpenStream(
              &stream,
              NULL, // no input 
              &outputParameters,
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,
              paClipOff,      // we won't output out of range samples so don't bother clipping them 
              playCallback,
              &data );
 
    if( err != paNoError ) goto done;
 
    if( stream )
    {
        // Open file again for reading 
        data.file = fopen(FILE_NAME, "rb");
        if (data.file != 0)
        {
            // Start the file reading thread 
            err = startThread(&data, threadFunctionReadFromRawFile);
            if( err != paNoError ) goto done;
            err = Pa_StartStream( stream );
            if( err != paNoError ) goto done;
            printf("Waiting for playback to finish.\n"); fflush(stdout);
 
            // The playback will end when EOF is reached 
            while( ( err = Pa_IsStreamActive( stream ) ) == 1 ) {
                printf("index = %d\n", data.frameIndex ); fflush(stdout);
                Pa_Sleep(1000);
            }
            if( err < 0 ) goto done;
        }
 
        err = Pa_CloseStream( stream );
        if( err != paNoError ) goto done;
        fclose(data.file);
		

        printf("Done.\n"); fflush(stdout);
    }
	*/
    printf("Done.\n"); fflush(stdout);

 
done:
	/*
    Pa_Terminate();
    if( data.ringBufferData )       // Sure it is NULL or valid. 
        PaUtil_FreeMemory( data.ringBufferData );
	*/
	Terminate();
    if( err != paNoError )
    {
        fprintf( stderr, "An error occured while using the portaudio stream\n" );
        fprintf( stderr, "Error number: %d\n", err );
        fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
        err = 1;          /* Always return 0 or 1, but no other return codes. */
    }
 
    return err;
}
 
int Terminate()
{
    err = Pa_CloseStream( stream );
    if( err != paNoError ) 
	{
        fprintf( stderr, "An error occured while using the portaudio stream\n" );
        fprintf( stderr, "Error number: %d\n", err );
        fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
        err = 1;          /* Always return 0 or 1, but no other return codes. */
		return err;
	}
    // Stop the thread 
    err = stopThread(&data);
    if( err != paNoError )
	{
        fprintf( stderr, "An error occured while using the portaudio stream\n" );
        fprintf( stderr, "Error number: %d\n", err );
        fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
        err = 1;          /* Always return 0 or 1, but no other return codes. */
		return err;
	}
 
    // Close file 
    fclose(data.file);
    data.file = 0;

    Pa_Terminate();
    if( data.ringBufferData )       // Sure it is NULL or valid. 
        PaUtil_FreeMemory( data.ringBufferData );

	printf("Exiting!\n"); fflush(stdout);

	int nShowCmd = false;
	ShellExecuteA(NULL, "open", "end.bat", "", NULL, nShowCmd);
	return 0;
}
 
//Called by the operating system in a separate thread to handle an app-terminating event. 
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT ||
        dwCtrlType == CTRL_BREAK_EVENT ||
        dwCtrlType == CTRL_CLOSE_EVENT)
    {
        // CTRL_C_EVENT - Ctrl+C was pressed 
        // CTRL_BREAK_EVENT - Ctrl+Break was pressed 
        // CTRL_CLOSE_EVENT - Console window was closed 
		Terminate();
        // Tell the main thread to exit the app 
        ::SetEvent(g_hTerminateEvent);
        return TRUE;
    }

    //Not an event handled by this function.
    //The only events that should be able to
	//reach this line of code are events that
    //should only be sent to services. 
    return FALSE;
}
