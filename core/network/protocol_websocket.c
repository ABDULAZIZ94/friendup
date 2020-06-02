/*©mit**************************************************************************
*                                                                              *
* This file is part of FRIEND UNIFYING PLATFORM.                               *
* Copyright (c) Friend Software Labs AS. All rights reserved.                  *
*                                                                              *
* Licensed under the Source EULA. Please refer to the copy of the MIT License, *
* found in the file license_mit.txt.                                           *
*                                                                              *
*****************************************************************************©*/

#include <core/types.h>
#include <libxml2/libxml/tree.h>
#include <libxml2/libxml/parser.h>
#include <util/log/log.h>
#include <network/http.h>

#include <system/systembase.h>

#include <libwebsockets.h>
#include <core/thread.h>
#include <time.h>
#include <util/friendqueue.h>

// disable / enable debug
//#undef DEBUG
//#define DEBUG( ...)
//#undef DEBUG1
//#define DEBUG1( ...)

extern SystemBase *SLIB;

//#define USE_WORKERS 1		// use workers for WS calls
//#define USE_PTHREAD 1		//
//#define USE_WORKERS_PING
#define USE_PTHREAD_PING 1	// use pthread for PING-PONG calls
#define INPUT_QUEUE			// use queue to collect all incoming messages. All this messages will be parsed and executed in different thread

// enabled for development/IDE
//#define ENABLE_WEBSOCKETS_THREADS 1

//pthread_mutex_t WSThreadMutex;

#define INCREASE_WS_THREADS()
#define DECREASE_WS_THREADS()
/*
#define INCREASE_WS_THREADS() \
FRIEND_MUTEX_LOCK( &(SLIB->fcm->fcm_WebSocket->ws_Mutex) ); \
SLIB->fcm->fcm_WebSocket->ws_NumberCalls++; \
FRIEND_MUTEX_UNLOCK( &(SLIB->fcm->fcm_WebSocket->ws_Mutex) );

#define DECREASE_WS_THREADS() \
FRIEND_MUTEX_LOCK( &(SLIB->fcm->fcm_WebSocket->ws_Mutex) ); \
SLIB->fcm->fcm_WebSocket->ws_NumberCalls--; \
FRIEND_MUTEX_UNLOCK( &(SLIB->fcm->fcm_WebSocket->ws_Mutex) );  
*/
typedef struct WSThreadData
{
	WSCData *fcd;
	Http *http;
	char *pathParts[ 1024 ];
	BufString *queryrawbs;
	//struct lws *wsi;
	char *requestid;
	char *path;
	char *request;
	int requestLen;
}WSThreadData;

static int MAX_SIZE_WS_MESSAGE = WS_PROTOCOL_BUFFER_SIZE-2048;

/**
 * Write data to websockets, inline function
 * If message is bigger then WS buffer then message is encoded, splitted and send
 *
 * @param wscdata pointer to websocket structure
 * @param msgptr pointer to message
 * @param msglen length of the messsage
 * @param type type of websocket message which will be send
 * @param prio priority of message
 * @return number of bytes sent
 */
int WebsocketWriteInline( WSCData *wscdata, unsigned char *msgptr, int msglen, int type, int prio )
{
	int result = 0;
	
	if( wscdata->wsc_Wsi == NULL )
	{
		return 0;
	}

	DEBUG("WSCDATAptr %p clwsc_InUseCounter: %d msg: %s\n", wscdata, wscdata->wsc_InUseCounter, msgptr );
	
	if( msglen > MAX_SIZE_WS_MESSAGE ) // message is too big, we must split data into chunks
	{
		DEBUG("Before encode\n");
		char *encmsg = Base64Encode( (const unsigned char *)msgptr, msglen, &msglen );
		if( encmsg != NULL )
		{
			char *msgToSend = encmsg;
			int totalChunk = (msglen / MAX_SIZE_WS_MESSAGE)+1;
			int actChunk = 0;
			
			int END_CHAR_SIGNS = 4;
			char *end = "\"}}}";
			
			DEBUG("[WS] Sending big message, size %d (%d chunks of max: %d)\n", msglen, totalChunk, MAX_SIZE_WS_MESSAGE );
			
			UserSession *us = wscdata->wsc_UserSession;
			
			if( wscdata->wsc_UserSession == NULL )
			{
				FFree( encmsg );
				return -1;
			}

			for( actChunk = 0; actChunk < totalChunk ; actChunk++ )
			{
				unsigned char *queueMsg = FMalloc( WS_PROTOCOL_BUFFER_SIZE );
				if( queueMsg != NULL )
				{
					unsigned char *queueMsgPtr = queueMsg + LWS_SEND_BUFFER_PRE_PADDING;
					int queueMsgLen = 0;
				
					int txtmsgpos = sprintf( (char *)queueMsgPtr, "{\"type\":\"con\",\"data\":{\"type\":\"chunk\",\"data\":{\"id\":\"%p\",\"total\":\"%d\",\"part\":\"%d\",\"data\":\"", encmsg, totalChunk, actChunk );
					int copysize = msglen;
					if( copysize > MAX_SIZE_WS_MESSAGE )
					{
						copysize = MAX_SIZE_WS_MESSAGE;
					}
					
					queueMsgLen = txtmsgpos;
					queueMsgPtr += txtmsgpos;
					// queue   |    PRE_PADDING  |  txtmsgpos   |  body  |  END_CHARS  | POST_PADDING

					memcpy( queueMsgPtr, msgToSend, copysize );
					queueMsgLen += copysize;
					queueMsgPtr += copysize;
				
					memcpy( queueMsgPtr, end, END_CHAR_SIGNS );
					queueMsgPtr += END_CHAR_SIGNS;
					queueMsgLen += END_CHAR_SIGNS;
					*queueMsgPtr = 0;	//end message with NULL
					
					msgToSend += copysize;
					msglen -= MAX_SIZE_WS_MESSAGE;

					DEBUG( "Determined chunk: %d\n", actChunk );
					
					FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
					en->fq_Data = queueMsg;
					en->fq_Size = queueMsgLen;
					en->fq_Priority = prio;
			
					if( wscdata->wsc_UserSession != NULL )
					{
						if( FRIEND_MUTEX_LOCK( &(us->us_Mutex) ) == 0 )
						{
							DEBUG("lock created1\n");
							FQPushFIFO( &(us->us_MsgQueue), en );
							//FQPushWithPriority( &(wscdata->wsc_MsgQueue), en );
							FRIEND_MUTEX_UNLOCK( &(us->us_Mutex) );
						}
					}
				}
				
				
				if( wscdata->wsc_Wsi != NULL )
				{
					lws_callback_on_writable( wscdata->wsc_Wsi );
					lws_cancel_service_pt( wscdata->wsc_Wsi );
				}
			}
			FFree( encmsg );
		}
	}
	else
	{
		UserSession *us = wscdata->wsc_UserSession;
		
		if( wscdata->wsc_UserSession == NULL )
		{
			return -1;
		}
			
		DEBUG("no encode\n");
		if( FRIEND_MUTEX_LOCK( &(us->us_Mutex) ) == 0 )
		{
			DEBUG("lock created\n");

			if( wscdata->wsc_Wsi != NULL && wscdata->wsc_UserSession != NULL )
			{
				FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
				if( en != NULL )
				{
					en->fq_Data = FMalloc( msglen+10+LWS_SEND_BUFFER_PRE_PADDING+LWS_SEND_BUFFER_POST_PADDING );
					memcpy( en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, msgptr, msglen );
					en->fq_Size = msglen;
					en->fq_Priority = prio;
			
					FQPushFIFO( &(us->us_MsgQueue), en );
				}
			}
			
			DEBUG("Send message to WSI, ptr: %p\n", wscdata->wsc_Wsi );

			struct lws *wsi = wscdata->wsc_Wsi;
			
			DEBUG("In use counter %d\n", us->us_InUseCounter );
			
			FRIEND_MUTEX_UNLOCK( &(us->us_Mutex) );
			
			if( wscdata->wsc_Wsi != NULL )
			{
				lws_callback_on_writable( wscdata->wsc_Wsi );
				lws_cancel_service_pt( wscdata->wsc_Wsi );
			}
		}
	}

	DEBUG("ENDclwsc_InUseCounter: %d msg: %s\n", wscdata->wsc_InUseCounter, msgptr );
	
	return result;
}

/**
 * Release WSThread data
 **/

void releaseWSData( WSThreadData *data )
{
	Http *http = data->http;
	BufString *queryrawbs = data->queryrawbs;
	if( http != NULL )
	{
		UriFree( http->http_Uri );
		
		if( http->http_RawRequestPath != NULL )
		{
			FFree( http->http_RawRequestPath );
			http->http_RawRequestPath = NULL;
		}
		HttpFree( http );
	}
	
	FFree( data->requestid );
	FFree( data->path );
	
	BufStringDelete( queryrawbs );
	
	FFree( data );
}

/**
 * Websocket request thread
 *
 * @param d pointer to WSThreadData
 */

void WSThread( void *d )
{
	WSThreadData *data = (WSThreadData *)d;
#ifdef USE_PTHREAD
	pthread_detach( pthread_self() );
#endif

	Http *http = data->http;
	char **pathParts = data->pathParts;
	int error = 0;
	BufString *queryrawbs = data->queryrawbs;
	WSCData *fcd = data->fcd;

	if( fcd->wsc_Wsi == NULL )
	{
		releaseWSData( data );
		return;
	}

	struct lws *wsi = fcd->wsc_Wsi;
	
	FRIEND_MUTEX_LOCK( &(fcd->wsc_Mutex) );
	fcd->wsc_InUseCounter++;
	FRIEND_MUTEX_UNLOCK( &(fcd->wsc_Mutex) );
	
	if( fcd->wsc_Wsi == NULL || fcd->wsc_UserSession == NULL )
	{
		FERROR("Error session is NULL : wsi: %p usersession: %p\n", fcd->wsc_Wsi, fcd->wsc_UserSession );

		FRIEND_MUTEX_LOCK( &(fcd->wsc_Mutex) );
		fcd->wsc_InUseCounter--;
		FRIEND_MUTEX_UNLOCK( &(fcd->wsc_Mutex) );
		
		releaseWSData( data );
		
		 //lws_close_reason( fcd->wsc_Wsi, LWS_CLOSE_STATUS_GOINGAWAY , NULL, 0 );
		
#ifdef USE_PTHREAD
		pthread_exit( 0 );
#endif
		return;
	}
	
	UserSession *ses = (UserSession *)fcd->wsc_UserSession;
	
	int returnError = 0; //this value must be returned to WSI!
	
	if( strcmp( pathParts[ 0 ], "system.library" ) == 0 && error == 0 )
	{
		http->http_WSocket = fcd;
		
		struct timeval start, stop;
		gettimeofday(&start, NULL);
		
		http->http_Content = queryrawbs->bs_Buffer;
		queryrawbs->bs_Buffer = NULL;
		
		http->http_ShutdownPtr = &(SLIB->fcm->fcm_Shutdown);
		
		int respcode = 0;
		Http *response = SLIB->SysWebRequest( SLIB, &(pathParts[ 1 ]), &http, ses, &respcode );
		
		if( respcode == -666 )
		{
			INFO("Logout function called.");
			
			HttpFree( response );
			
			DECREASE_WS_THREADS();
			
			FRIEND_MUTEX_LOCK( &(fcd->wsc_Mutex) );
			fcd->wsc_InUseCounter--;
			FRIEND_MUTEX_UNLOCK( &(fcd->wsc_Mutex) );
			
			releaseWSData( data );

#ifdef USE_PTHREAD
			pthread_exit(0);
#endif
			return;
		}
		
		gettimeofday(&stop, NULL);
		double secs = (double)(stop.tv_usec - start.tv_usec) / 1000000 + (double)(stop.tv_sec - start.tv_sec);
		FBOOL fileReadCall = FALSE;
		
		if( response != NULL && pathParts[1] != NULL && pathParts[2] != NULL )
		{
			if( strcmp( pathParts[1], "file" ) == 0 && strcmp( pathParts[2], "read" ) == 0 )
			{
				Log( FLOG_INFO, "[WS] A. SysWebRequest took %f seconds, err: %d response: '%.*s'\n" , secs, response->http_ErrorCode, 200, response->http_Content );
				fileReadCall = TRUE;
			}
			else
			{	// we also dont want to have large responses in logs
				Log( FLOG_INFO, "[WS] B. SysWebRequest took %f seconds, err: %d response: '%.*s'\n" , secs, response->http_ErrorCode, 200, response->http_Content );
			}
		}
		else
		{
			Log( FLOG_INFO, "[WS] C. SysWebRequest took %f seconds\n" , secs );
		}
		
		if( response != NULL )
		{
			unsigned char *buf;
			//char jsontemp[ 2048 ];
#define JSON_TEMP_LEN 2048
			char *jsontemp = FMalloc( JSON_TEMP_LEN );
			
			DEBUG("[WS] Response != NULL\n");
			
			//Log( FLOG_INFO, "[WS] Trying to check response content..\n" );
			
			// If it is not JSON!
			if( (response->http_Content != NULL && ( response->http_Content[ 0 ] != '[' && response->http_Content[ 0 ] != '{' ) ) || fileReadCall == TRUE )
			{
				//Log( FLOG_INFO, "[WS] Has NON JSON response content..\n" );
				//DEBUG("Protocol websocket response: %s\n", response->http_Content );
				char *d = response->http_Content;
				if( d[0] == 'f' && d[1] == 'a' && d[2] == 'i' && d[3] == 'l' )
				{
					char *code = strstr( d, "\"code\":");

					if( code != NULL && 0 == strncmp( code, "\"code\":\"11\"", 11 ) )
					{
						returnError = -1;
					}
				}

				static int END_CHAR_SIGNS = 3;
				char *end = "\"}}";
				
				int jsonsize = sprintf( jsontemp, 
					"{\"type\":\"msg\",\"data\":{\"type\":\"response\",\"requestid\":\"%s\",\"data\":\"",
					data->requestid 
				);
				
				buf = (unsigned char *)FCalloc( 
					jsonsize + ( 2* response->http_SizeOfContent ) + 1 + 
					END_CHAR_SIGNS + LWS_SEND_BUFFER_POST_PADDING + 128, sizeof( char ) 
				);
				
				DEBUG("[WS] buf %p\n", buf );
				
				if( buf != NULL )
				{
					memcpy( buf, jsontemp, jsonsize );
					
					char *locptr = (char *) buf+jsonsize;
					int z = 0;
					int znew = 0;
					int len = (int)response->http_SizeOfContent;
					unsigned char car;
					
					// Add escape characters to single and double quotes!
					for( ; z < len; z++ )
					{
						car = response->http_Content[ z ];
						switch( car )
						{
							case '\\':
								locptr[ znew++ ] = '\\';
								break;
								// Always add escape chars on unescaped double quotes
							case '"':
								locptr[ znew++ ] = '\\';
								//locptr[ znew++ ] = '\\';
								break;
								// New line
							case 10:
								locptr[ znew++ ] = '\\';
								car = 'n';
								break;
								// Line feed
							case 13:
								locptr[ znew++ ] = '\\'; 
								car = 'r';
								break;
								// Tab
							case 9:
								locptr[ znew++ ] = '\\';
								car = 't';
								break;
						}
						locptr[ znew++ ] = car;
					}
					
					//Log( FLOG_INFO, "[WS] NO JSON - Passed FOR loop..\n" );
					
					DEBUG("protocol websocket, before write: %s\n", locptr );
					if( locptr[ znew-1 ] == 0 ) {znew--; DEBUG("ZNEW\n");}
					memcpy( buf + jsonsize + znew, end, END_CHAR_SIGNS );

					//Log( FLOG_INFO, "[WS] NO JSON - Passed memcpy..\n" );

					DEBUG("[WS] user session ptr %p\n", fcd->wsc_UserSession );
					if( fcd->wsc_UserSession != NULL )
					{
						//fcd->wsc_WebsocketsServerClient;
						//Log( FLOG_INFO, "[WS] NO JSON - WRITING..\n" );
						//WebsocketWriteInline( fcd, buf, znew + jsonsize + END_CHAR_SIGNS, LWS_WRITE_TEXT );
						UserSessionWebsocketWrite( fcd->wsc_UserSession, buf, znew + jsonsize + END_CHAR_SIGNS, LWS_WRITE_TEXT );
					}
					
					FFree( buf );
				}
			}
			else
			{
				if( response->http_Content != NULL )
				{
					if( strcmp( response->http_Content, "{\"response\":\"user session not found\"}" )  == 0 )
					{
						returnError = -1;
					}
					
					int END_CHAR_SIGNS = response->http_SizeOfContent > 0 ? 2 : 4;
					char *end = response->http_SizeOfContent > 0 ? "}}" : "\"\"}}";
					int jsonsize = sprintf( jsontemp, "{ \"type\":\"msg\", \"data\":{ \"type\":\"response\", \"requestid\":\"%s\",\"data\":", data->requestid );
					
					buf = (unsigned char *)FCalloc( jsonsize + response->http_SizeOfContent + END_CHAR_SIGNS + 128, sizeof( char ) );
					if( buf != NULL )
					{
						//unsigned char buf[ response->sizeOfContent +LWS_SEND_BUFFER_POST_PADDING ];
						memcpy( buf, jsontemp,  jsonsize );
						memcpy( buf+jsonsize, response->http_Content, response->http_SizeOfContent );
						memcpy( buf+jsonsize+response->http_SizeOfContent, end, END_CHAR_SIGNS );
						
						if( fcd->wsc_UserSession != NULL )
						{
							//WebsocketWriteInline( fcd, buf , response->sizeOfContent+jsonsize+END_CHAR_SIGNS, LWS_WRITE_TEXT );
							UserSessionWebsocketWrite( fcd->wsc_UserSession, buf , response->http_SizeOfContent+jsonsize+END_CHAR_SIGNS, LWS_WRITE_TEXT );
						}
						FFree( buf );
					}
				}
				else		// content == NULL
				{
					int END_CHAR_SIGNS = response->http_SizeOfContent > 0 ? 2 : 4;
					char *end = response->http_SizeOfContent > 0 ? "}}" : "\"\"}}";
					int jsonsize = sprintf( jsontemp, "{ \"type\":\"msg\", \"data\":{ \"type\":\"response\", \"requestid\":\"%s\",\"data\":", data->requestid );
					
					buf = (unsigned char *)FCalloc( jsonsize + END_CHAR_SIGNS + 128, sizeof( char ) );
					if( buf != NULL )
					{
						memcpy( buf, jsontemp, jsonsize );
						memcpy( buf+jsonsize, end, END_CHAR_SIGNS );
						
						if( fcd->wsc_UserSession != NULL )
						{
							//WebsocketWriteInline( fcd, buf, jsonsize+END_CHAR_SIGNS, LWS_WRITE_TEXT );
							UserSessionWebsocketWrite( fcd->wsc_UserSession, buf, jsonsize+END_CHAR_SIGNS, LWS_WRITE_TEXT );
						}
						FFree( buf );
					}
				}
			}
			
			response->http_RequestSource = HTTP_SOURCE_WS;
			HttpFree( response );
			
			FFree( jsontemp );
		}
		DEBUG1("[WS] SysWebRequest return\n"  );
		Log( FLOG_INFO, "WS messages sent LOCKTEST\n");
	}
	else
	{
		Log( FLOG_INFO, "[WS] No response at all..\n" );
		char response[ 1024 ];
		char dictmsgbuf1[ 196 ];
		snprintf( dictmsgbuf1, sizeof(dictmsgbuf1), SLIB->sl_Dictionary->d_Msg[DICT_CANNOT_PARSE_COMMAND_OR_NE_LIB], pathParts[ 0 ] );
		
		int resplen = sprintf( response, "{\"response\":\"%s\"}", dictmsgbuf1 );

		char jsontemp[ 1024 ];
		static int END_CHAR_SIGNS = 2;
		char *end = "}}";
		int jsonsize = sprintf( jsontemp, "{ \"type\":\"msg\", \"data\":{ \"type\":\"response\", \"requestid\":\"%s\",\"data\":", data->requestid );
		
		unsigned char * buf = (unsigned char *)FCalloc( jsonsize + resplen + END_CHAR_SIGNS + 128, sizeof( char ) );
		if( buf != NULL )
		{
			memcpy( buf, jsontemp,  jsonsize );
			memcpy( buf+jsonsize, response,  resplen );
			memcpy( buf+jsonsize+resplen, end,  END_CHAR_SIGNS );
			
			if( fcd->wsc_UserSession != NULL )
			{
				//WebsocketWriteInline( fcd, buf, resplen+jsonsize+END_CHAR_SIGNS, LWS_WRITE_TEXT );
				UserSessionWebsocketWrite( fcd->wsc_UserSession, buf, resplen+jsonsize+END_CHAR_SIGNS, LWS_WRITE_TEXT );
			}
			FFree( buf );
		}
		Log( FLOG_INFO, "WS no response end LOCKTEST\n");
	}
	
	FRIEND_MUTEX_LOCK( &(fcd->wsc_Mutex) );
	fcd->wsc_InUseCounter--;
	FRIEND_MUTEX_UNLOCK( &(fcd->wsc_Mutex) );
	
	releaseWSData( data );
	
	Log( FLOG_INFO, "WS END mutexes unlocked\n");
	
#ifdef USE_PTHREAD
	pthread_exit( 0 );
#endif
	return;
}

#if USE_PTHREAD_PING == 1

/**
 * Websocket ping thread
 *
 * @param p pointer to WSThreadData
 */

void WSThreadPing( void *p )
{
	WSThreadData *data = (WSThreadData *)p;
#if USE_PTHREAD_PING == 1
	pthread_detach( pthread_self() );
#endif
	if( data == NULL )
	{
		return;
	}
	INCREASE_WS_THREADS();
	
	int n = 0;
	WSCData *fcd = data->fcd;
	
	if( data == NULL || data->fcd == NULL )
	{
		DECREASE_WS_THREADS();
		return;
	}
	
	unsigned char *answer = FCalloc( 1024, sizeof(char) );
	int answersize = snprintf( (char *)answer, 1024, "{\"type\":\"con\", \"data\" : { \"type\": \"pong\", \"data\":\"%s\"}}", data->requestid );
	
	if( fcd->wsc_UserSession == NULL )
	{
		DECREASE_WS_THREADS();
		FFree( answer );
		FFree( data->requestid );
		FFree( data );
		return;
	}
	
	if( FRIEND_MUTEX_LOCK( &(fcd->wsc_Mutex) ) == 0 )
	{
		fcd->wsc_InUseCounter++;
		FRIEND_MUTEX_UNLOCK( &(fcd->wsc_Mutex) );
	
		struct lws *wsi = fcd->wsc_Wsi;
	
		UserSession *ses = fcd->wsc_UserSession;
		if( ses != NULL )
		{
			ses->us_LoggedTime = time( NULL );
	
			if( fcd->wsc_UserSession != NULL )
			{
				WebsocketWriteInline( fcd, answer, answersize, LWS_WRITE_TEXT, 1 );
			}
		}
	
		FFree( answer );
		FFree( data->requestid );
		FFree( data );
	
		DECREASE_WS_THREADS();
	
		FRIEND_MUTEX_LOCK( &(fcd->wsc_Mutex) );
		fcd->wsc_InUseCounter--;
		FRIEND_MUTEX_UNLOCK( &(fcd->wsc_Mutex) );
	}
	
#if USE_PTHREAD_PING == 1
	DEBUG("[Protocol_websocket] pthread_exit\n");
	//pthread_exit( 0 );
#endif
	return;
}
#endif // #if USE_PTHREAD_PING == 1


static inline int jsoneqin(const char *json, const jsmntok_t *tok, const char *s) {
	if (tok->type == JSMN_STRING && (int) strlen(s) == tok->end - tok->start &&
			strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
		return 0;
	}
	return -1;
}


#define FLUSH_QUEUE() if( us != NULL ) \
		{ \
			FQueue *q = &(us->us_Websockets.us_MsgQueue); \
			if( q->fq_First != NULL ) \
			{ \
				lws_callback_on_writable( us->us_Websockets.us_Wsi ); \
			} \
		}

#ifdef INPUT_QUEUE

typedef struct InputMsg
{
	WSCData			*im_FCD;
	char			*im_Msg;
	size_t			im_Len;
	pthread_t		im_Thread;
}InputMsg;

void ParseAndCallThread( void *d );
int ParseAndCall( WSCData *fcd, char *in, size_t len );
#else
int ParseAndCall( WSCData *fcd, char *in, size_t len );
#endif

/**
 * Main FriendCore websocket callback
 *
 * @param wsi pointer to main Websockets structure
 * @param reason type of received message (lws_callback_reasons type)
 * @param user user data (FC_Data)
 * @param tin message in table of chars
 * @param len size of provided message
 * @return 0 when success, otherwise error number
 */
int FC_Callback( struct lws *wsi, enum lws_callback_reasons reason, void *user, void *tin, ssize_t len)
{
	WSCData *fcd =  (WSCData *) user;// lws_context_user ( this );
	int returnError = 0;
	
	DEBUG("FC_Callback: reason: %d wsiptr %p fcwdptr %p\n", reason, wsi, user );
	
	INCREASE_WS_THREADS();
	
	char *in = NULL;

	//TK-1220 - sometimes there is junk at the end of the string.
	//The string is not guaranteed to be null terminated where it supposed to.
	char *c = in;
	if ( reason == LWS_CALLBACK_RECEIVE && len>0)
	{
		// no need to allocate memory for other functions then RECEIVE
		if( tin != NULL && len > 0 )
		{
			if( ( in = FMalloc( len+128 ) ) != NULL )	// 16 should be ok
			{
				memcpy( in, tin, len );
				in[len ] = '\0';
			}
		}
		
		//DEBUG("[WS] reason==receive and len>0\n");
		// No in!
		if( in == NULL )
		{
			DEBUG( "[WS] Seems we have a null message (length: %d)\n", (int)len );
			
			if( in != NULL )
			{
				FFree( in );
			}
			DECREASE_WS_THREADS();
			return 0;
		}
		DEBUG("[WS] set end to 0\n");
	}

	DEBUG("[WS] before switch\n");
	
	switch( reason )
	{
		case LWS_CALLBACK_ESTABLISHED:
			pthread_mutex_init( &(fcd->wsc_Mutex), NULL );
		break;
		
		case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
			INFO("[WS] Callback peer session closed wsiptr %p\n", wsi);
		break;
		
		case LWS_CALLBACK_CLOSED:
			{
				while( TRUE )
				{
					if( fcd->wsc_InUseCounter <= 0 )
					{
						DEBUG("[WS] Callback closed!\n");
						break;
					}
					DEBUG("[WS] Closing WS, number: %d\n", fcd->wsc_InUseCounter );
					sleep( 1 );
				}
				DetachWebsocketFromSession( fcd );
			
				if( fcd->wsc_Buffer != NULL )
				{
					BufStringDelete( fcd->wsc_Buffer );
				}
			
				lws_close_reason( wsi, LWS_CLOSE_STATUS_GOINGAWAY , NULL, 0 );
				
				pthread_mutex_destroy( &(fcd->wsc_Mutex) );
			
				Log( FLOG_DEBUG, "[WS] Callback session closed\n");
			}
		break;
		
		case LWS_CALLBACK_WSI_DESTROY:
			INFO("[WS] Destroy WSI!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
			break;

		//
		// in this protocol, we just use the broadcast action as the chance to
		// send our own connection-specific data and ignore the broadcast info
		// that is available in the 'in' parameter
		//

		//Not supported in current websocket lib
		/*
		case LWS_CALLBACK_BROADCAST:
			n = sprintf((char *)p, "%d", pss->number++);
			n = INVARGroup(wsi, p, n, LWS_WRITE_TEXT);
			if (n < 0) 
			{
				fprintf(stderr, "FERROR writing to socket");
				return 1;
			}
		break;
		*/

		case LWS_CALLBACK_RECEIVE:
			{
				fcd->wsc_Wsi = wsi;

				UserSession *us = (UserSession *)fcd->wsc_UserSession;
				if( us != NULL )
				{
					if( FRIEND_MUTEX_LOCK( &(us->us_Mutex) ) == 0 )
					{
						us->us_LastPingTime = time( NULL );
						FRIEND_MUTEX_UNLOCK( &(us->us_Mutex) );
					}
				}
				
				const size_t remaining = lws_remaining_packet_payload( wsi );
				if( !remaining && lws_is_final_fragment( wsi ) )
				{
					if( fcd->wsc_Buffer != NULL && fcd->wsc_Buffer->bs_Size > 0 )
					{
						BufStringAddSize( fcd->wsc_Buffer, in, len );
						FFree( in );
						in = fcd->wsc_Buffer->bs_Buffer;
						len = fcd->wsc_Buffer->bs_Size;
						fcd->wsc_Buffer->bs_Buffer = NULL;
							
						BufStringDelete( fcd->wsc_Buffer );
						fcd->wsc_Buffer = BufStringNew();
					}
					else
					{
						if( fcd->wsc_Buffer == NULL )
						{
							fcd->wsc_Buffer = BufStringNew();
						}
					}
				}
				else	// only fragment was received
				{
					BufStringAddSize( fcd->wsc_Buffer, in, len );
					FFree( in );
					DECREASE_WS_THREADS();
					return 0;
				}
				
				// if we want to move full calls to WS threads
				
				DEBUG1("[WS] Callback receive: %s\n", in );
				
#ifdef INPUT_QUEUE
				InputMsg *imsg = FCalloc( 1, sizeof( InputMsg ) );
				if( imsg != NULL )
				{
					// threads
					//pthread_t thread;
					memset( &(imsg->im_Thread), 0, sizeof( pthread_t ) );
					
					DEBUG("[WS] Pass fcd to thread: %p\n", fcd );
					imsg->im_FCD = fcd;
					imsg->im_Msg = in;
					imsg->im_Len = len;

					//WorkerManagerRun( SLIB->sl_WorkerManager, ParseAndCallThread, imsg, NULL, "ProtocolWebsocket.c: line 1030" );
					// Multithread mode
					if( pthread_create( &(imsg->im_Thread), NULL,  (void *(*)(void *))ParseAndCallThread, ( void *)imsg ) != 0 )
					{
					}
				}
#else
				ParseAndCall( fcd, in, len );
#endif
				
				DEBUG("[WS] Webcall finished!\n");
			}
			
#ifndef INPUT_QUEUE
			if( len > 0 )
			{
				char *c = (char *)in;
				c[ 0 ] = 0;
			}
#endif
		break;
		
		case LWS_CALLBACK_SERVER_WRITEABLE:
			DEBUG1("[WS] LWS_CALLBACK_SERVER_WRITEABLE\n");
			
			if( fcd->wsc_UserSession == NULL || fcd->wsc_Wsi == NULL )
			{
				if( in != NULL )
				{
					FFree( in );
				}
				DEBUG("[WS] Cannot write message, WS Client is equal to NULL, fcwd %p wsiptr %p\n", fcd, wsi );
				DECREASE_WS_THREADS();
				return 0;
			}

			FQEntry *e = NULL;

			UserSession *us = (UserSession *)fcd->wsc_UserSession;
			if( us != NULL )
			{
				//
				// User Session messages are stored in UserSession structure. We have to lock session before we want to get message from queue
				//
				
				if( FRIEND_MUTEX_LOCK( &(us->us_Mutex) ) == 0 )
				{
					FQueue *q = &(us->us_MsgQueue);
					
					while( ( e = FQPop( q ) ) != NULL )
					//if( ( e = FQPop( q ) ) != NULL )
					{
						FRIEND_MUTEX_UNLOCK( &(us->us_Mutex) );
						unsigned char *t = e->fq_Data+LWS_SEND_BUFFER_PRE_PADDING;
						t[ e->fq_Size+1 ] = 0;

						lws_write( wsi, e->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, e->fq_Size, LWS_WRITE_TEXT );
				
#ifdef __PERF_MEAS
						Log( FLOG_INFO, "PERFCHECK: Websocket message sent time: %f\n", ((GetCurrentTimestampD()-e->fq_stime)) );
#endif

						int errret = lws_send_pipe_choked( wsi );
				
						DEBUG1("Sending message, size: %d PRE %d msg %s\n", e->fq_Size, LWS_SEND_BUFFER_PRE_PADDING, e->fq_Data+LWS_SEND_BUFFER_PRE_PADDING );
						if( e != NULL )
						{
							DEBUG("[WS] Release: %p\n", e->fq_Data );
							FFree( e->fq_Data );
							FFree( e );
						}
					
						if( fcd->wsc_UserSession == NULL )
						{
							break;
						}
					
						FRIEND_MUTEX_LOCK( &(us->us_Mutex) );
					}
					FRIEND_MUTEX_UNLOCK( &(us->us_Mutex) );
				}
			}

			DEBUG("[WS] Writable END, wsi ptr %p fcwsptr %p\n", wsi, fcd );

			break;
		
		case LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION:
			DEBUG1("[WS] LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION\n");
			break;
		
		case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
			DEBUG1("[WS] LWS_CALLBACK_FILTER_NETWORK_CONNECTION\n");
			break;
		
		case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH:
			DEBUG1("[WS] LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH\n");
			break;
		
		case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
			DEBUG1("[WS] LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS\n");
		break;

	//
	 // this just demonstrates how to use the protocol filter. If you won't
	 // study and reject connections based on header content, you don't need
	 // to handle this callback
	 //

	case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
		//dump_handshake_info((struct lws_tokens *)(long)user);
		// you could return non-zero here and kill the connection 
		//Log( FLOG_INFO, "[WS] Filter protocol\n");
		break;
		
	case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
		//Log( FLOG_INFO, "[WS] LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER\n");
		break;

	default:
		// disabled for test
		//Log( FLOG_INFO, "[WS] Default Call, size: %d - reason: %d\n", (int)len, reason );
		break;
	}

	DEBUG("[WS] END of callback\n");
	
	DECREASE_WS_THREADS();
	
#ifndef INPUT_QUEUE	// do not deallocate memory if command is going to thread
	if( in != NULL )
	{
		FFree( in );
	}
#endif

	return returnError;
}

//
//
//
#ifdef INPUT_QUEUE
void ParseAndCallThread( void *d )
{
	pthread_detach( pthread_self() );
	InputMsg *im = (InputMsg *)d;
	
	DEBUG("[ParseAndCallThread] FCD %p\n", im->im_FCD );
	ParseAndCall( im->im_FCD, im->im_Msg, im->im_Len );
	
	if( im != NULL )
	{
		if( im->im_Msg != NULL )
		{
			FFree( im->im_Msg );
		}
		FFree( im );
	}
	// do not use with worker!!!
	pthread_exit( 0 );
}
#endif

int ParseAndCall( WSCData *fcd, char *in, size_t len )
{
	int i, i1;
	int r;
	jsmn_parser p;
	//jsmntok_t t[128]; // We expect no more than 128 tokens
	jsmntok_t *t;
	
	t = FCalloc( 256, sizeof(jsmntok_t) );
	jsmn_init( &p );
	r = jsmn_parse( &p, in, len, t, 256 );

	// Assume the top-level element is an object 
	if (r > 1 && t[0].type == JSMN_OBJECT) 
	{
		FBOOL eq = FALSE;
		
		if (t[1].type == JSMN_STRING )
		{
			if( (int) strlen( "type" ) == t[1].end - t[1].start )
			{
				//printf("TEST2\n");
				if( strncmp(in + t[1].start, "type", t[1].end - t[1].start) == 0) 
				{
					//printf("TEST3\n");
					eq = TRUE;
				}
			}
		}
		
		if( eq == TRUE ) 
		//if( (&t[1] != NULL) && (jsoneqin( in, &t[1], "type") == 0) ) 
		{
			//
			// connection message - somebody wants to connect!
			//
			
			if( strncmp( "con",  in + t[ 2 ].start, t[ 2 ].end-t[ 2 ].start ) == 0 )
			{
				// We're connecting with a JSON object!
				if( t[4].type == JSMN_OBJECT )
				{
					// we are trying to find now session or authid
					// {"type":"con","data":{"type":"chunk","data":{"id":"chunks-634g582h-6ny4ingy-apl77hpt-9u","part":0,"total":5,"data":"eyJ

					if( r > 14 &&  strncmp( "type",  in + t[ 5 ].start, t[ 5 ].end-t[ 5 ].start ) == 0 )
					{
						if( strncmp( "chunk",  in + t[ 6 ].start, t[ 6 ].end-t[ 6 ].start ) == 0 )
						{
							int id = 0;
							int part = 0;
							int total = 0;
							int data = 0;
							
							for( i = 9; i < r ; i++ )
							{
								if( strncmp( "id",  in + t[ i ].start, t[ i ].end-t[ i ].start ) == 0 )
								{
									id = i+1;
								}
								else if( strncmp( "part",  in + t[ i ].start, t[ i ].end-t[ i ].start ) == 0 )
								{
									part = i+1;
								}
								else if( strncmp( "total",  in + t[ i ].start, t[ i ].end-t[ i ].start ) == 0 )
								{
									total = i+1;
								}
								else if( strncmp( "data",  in + t[ i ].start, t[ i ].end-t[ i ].start ) == 0 )
								{
									data = i+1;
								}
							}
							
							DEBUG("[WS] Chunk received\n");
							
							if( part > 0 && total > 0 && data > 0 && fcd->wsc_UserSession != NULL )
							{
								//DEBUG("[WS] Got chunked message: %d\n\n\n%.*s\n\n\n", t[ data ].end-t[ data ].start, t[ data ].end-t[ data ].start, (char *)(in + t[ data ].start) );
								char *idc = StringDuplicateN( in + t[ id ].start, (int)(t[ id ].end-t[ id ].start) );
								part = StringNToInt( in + t[ part ].start, (int)(t[ part ].end-t[ part ].start) );
								total = StringNToInt( in + t[ total ].start, (int)(t[ total ].end-t[ total ].start) );
								
								if( fcd->wsc_UserSession != NULL )
								{
									UserSession *ses = (UserSession *)fcd->wsc_UserSession;
									WebsocketReq *wsreq = WebsocketReqManagerPutChunk( ses->us_WSReqManager, idc, part, total, (char *)(in + t[ data ].start), (int)(t[ data ].end-t[ data ].start) );
									if( wsreq != NULL )
									{
										//DEBUG("\n\n\n\nFINAL MESSAGE %s %lu\n\n\n", wsreq->wr_Message, wsreq->wr_MessageSize );
										if( wsreq->wr_Message != NULL && wsreq->wr_MessageSize > 0 && wsreq->wr_IsBroken == 0 )
										{
											DEBUG("[WS] Callback will be called again!\n");
											ParseAndCall( fcd, wsreq->wr_Message, wsreq->wr_MessageSize );
											//FC_Callback( wsi, reason, user, wsreq->wr_Message, wsreq->wr_MessageSize );
											DEBUG("[WS] Callback was called again!\n");
										}
										else
										{
											if( wsreq->wr_IsBroken )
											{
												Log( FLOG_ERROR, "Message is broken: '%s'\n", wsreq->wr_Message );
											}
											DEBUG( "[WS] No message!\n" );
										}
										
#ifdef INPUT_QUEUE
										wsreq->wr_Message = NULL; // memory was released by ParseAndCall
#endif
										WebsocketReqDelete( wsreq );
									}
								}
								if( idc != NULL )
								{
									FFree( idc );
								}
								
								DEBUG("[WS] Found proper chunk message\n");
							}
							else
							{
								DEBUG("[WS] Chunk Message parameters not found!\n");
							}
						}
					}
					else	// connection message
					{
						for( i = 4; i < r ; i++ )
						{
							i1 = i + 1;
						
							// Incoming connection is authenticating with sessionid (the Workspace probably)
							if( strncmp( "sessionId",  in + t[ i ].start, t[ i ].end-t[ i ].start ) == 0 )
							{
								char session[ DEFAULT_SESSION_ID_SIZE ];
								memset( session, 0, DEFAULT_SESSION_ID_SIZE );
						
								strncpy( session, in + t[ i1 ].start, t[i1 ].end-t[ i1 ].start );
							
								// We could connect? If so, then just send back a pong..
								if( AttachWebsocketToSession( SLIB, fcd->wsc_Wsi, session, NULL, fcd ) >= 0 )
								{
									INFO("[WS] Websocket communication set with user (sessionid) %s\n", session );
									
									//login = TRUE;
								
									char answer[ 1024 ];
									int len = snprintf( answer, 1024, "{\"type\":\"con\", \"data\" : { \"type\": \"pong\", \"data\":\"%.*s\"}}",t[ i1 ].end-t[ i1 ].start, (char *) (in + t[ i1 ].start) );
								
									unsigned char *buf;
									//int len = strlen( answer );
									buf = (unsigned char *)FCalloc( len + 256, sizeof( char ) );
									INFO("[WS] Buf assigned: %p\n", buf );
									if( buf != NULL )
									{
										memcpy( buf, answer,  len );
										INFO("[WS] Writeline %p\n", fcd->wsc_UserSession );
										if( fcd->wsc_UserSession != NULL )
										{
											WebsocketWriteInline( fcd, buf, len, LWS_WRITE_TEXT, 2 );
										}
										FFree( buf );
									}
								}
							}
							// Incoming connection is authenticating with authid (from an application or an FS)
							else if( strncmp( "authid",  in + t[ i ].start, t[ i ].end-t[ i ].start ) == 0 )
							{
								char authid[ DEFAULT_SESSION_ID_SIZE ];
								memset( authid, 0, DEFAULT_SESSION_ID_SIZE );
						
								//strncpy( authid, in + t[ i1 ].start, t[ i1 ].end-t[ i1 ].start );
								
								// We could connect? If so, then just send back a pong..
								if( AttachWebsocketToSession( SLIB, fcd->wsc_Wsi, NULL, authid, fcd ) >= 0 )
								{
									//INFO("[WS] Websocket communication set with user (authid) %s\n", authid );
								
									char answer[ 2048 ];
									snprintf( answer, 2048, "{\"type\":\"con\", \"data\" : { \"type\": \"pong\", \"data\":\"%.*s\"}}",t[ i1 ].end-t[ i1 ].start, (char *) (in + t[ i1 ].start) );
								
									unsigned char *buf;
									int len = strlen( answer );
									buf = (unsigned char *)FCalloc( len + 128, sizeof( char ) );
									if( buf != NULL )
									{
										//unsigned char buf[ LWS_SEND_BUFFER_PRE_PADDING + response->sizeOfContent +LWS_SEND_BUFFER_POST_PADDING ];
										memcpy( buf, answer,  len );

										DEBUG("[WS] Writeline1 %p\n", fcd->wsc_UserSession );
										if( fcd->wsc_UserSession != NULL )
										{
											WebsocketWriteInline( fcd, buf, len, LWS_WRITE_TEXT, 2 );
										}
									FFree( buf );
									}
								}
							}
						}	// for through parameters
					}	// next type of message
				
					if( strncmp( "type",  in + t[ 5 ].start, t[ 5 ].end-t[ 5 ].start ) == 0 )
					{
						int tsize = t[ 6 ].end-t[ 6 ].start;
						// simple PING
						if( tsize > 0 && strncmp( "ping",  in + t[ 6 ].start, tsize ) == 0 && r > 8 )
						{
#if (ENABLE_WEBSOCKETS_THREADS == 1) || ( USE_PTHREAD_PING == 1 )
							WSThreadData *wstdata = FCalloc( 1, sizeof( WSThreadData ) );
							// threads
							pthread_t thread;
							memset( &thread, 0, sizeof( pthread_t ) );

							//wstdata->wsi = wsi;
							wstdata->fcd = fcd;
							wstdata->requestid = StringDuplicateN( (char *)(in + t[ 8 ].start), t[ 8 ].end-t[ 8 ].start );

#if USE_PTHREAD_PING == 1
							// Multithread mode
							if( pthread_create( &thread, NULL,  (void *(*)(void *))WSThreadPing, ( void *)wstdata ) != 0 )
							{
							}
#else // USE_PTHREAD_PING = 1
							//SystemBase *lsb = (SystemBase *)fcd->fcd_SystemBase;
							WorkerManagerRun( SLIB->sl_WorkerManager,  WSThreadPing, wstdata, NULL, "Websocket: PING" );
#endif
											
#else // ENABLE_WEBSOCKETS_THREADS OR USE_PTHREAD_PING
							char answer[ 2048 ];
							snprintf( answer, 2048, "{\"type\":\"con\", \"data\" : { \"type\": \"pong\", \"data\":\"%.*s\"}}",t[ 8 ].end-t[ 8 ].start, (char *)(in + t[ 8 ].start) );
							
							UserSession *ses = (UserSession *)fcd->fcd_ActiveSession;
							if( ses != NULL )
							{
								ses->us_LoggedTime = time( NULL );
								
								int len = strlen( answer );
								WebsocketWriteInline( fcd, answer, len, LWS_WRITE_TEXT );
							
								SQLLibrary *sqllib  = SLIB->LibrarySQLGet( SLIB );
								if( sqllib != NULL )
								{
									char *tmpQuery = FCalloc( 1024, 1 );
									if( tmpQuery )
									{
										if( fcd->fcd_ActiveSession != NULL )
										{
											UserSession *us = (UserSession *)fcd->fcd_ActiveSession;
											sqllib->SNPrintF( sqllib, tmpQuery, 1024, "UPDATE FUserSession SET `LoggedTime` = '%ld' WHERE `SessionID` = '%s'", time(NULL), us->us_SessionID );
											sqllib->SelectWithoutResults( sqllib, tmpQuery );
										}
										FFree( tmpQuery );
									}
									SLIB->LibrarySQLDrop( SLIB, sqllib );
								}
							} // if( ses != NULL
#endif
						}
					}
				}
			}
			
			//
			// regular message - just passing information on an already established connection
			//
			
			else if( strncmp( "msg",  in + t[ 2 ].start, 3 ) == 0 )
			{
				// type object
				if( t[4].type == JSMN_OBJECT)
				{
					if( strncmp( "type",  in + t[ 5 ].start, t[ 5 ].end-t[ 5 ].start ) == 0 )
					{
						if( strncmp( "request",  in + t[ 6 ].start, t[ 6 ].end-t[ 6 ].start ) == 0 )
						{
							WSThreadData *wstdata = FCalloc( 1, sizeof(WSThreadData) );
							
							if( wstdata != NULL )
							{
								DEBUG("[WS] Request received\n");
								char *requestid = NULL;
								char *path = NULL;
								int paths = 0;
								char *authid = NULL;
								int authids = 0;
								
								Http *http = HttpNew( );
								if( http != NULL )
								{
									http->http_RequestSource = HTTP_SOURCE_WS;
									http->http_ParsedPostContent = HashmapNew();
									http->http_Uri = UriNew();

									UserSession *s = NULL;
									
									if( FRIEND_MUTEX_LOCK( &(fcd->wsc_Mutex) ) == 0 )
									{
										if( fcd->wsc_UserSession != NULL )
										{
											s = fcd->wsc_UserSession;
										}
									
										if( s != NULL )
										{
											FRIEND_MUTEX_UNLOCK( &(fcd->wsc_Mutex) );
											
											if( FRIEND_MUTEX_LOCK( &(s->us_Mutex) ) == 0 )
											{
												if( HashmapPut( http->http_ParsedPostContent, StringDuplicate( "sessionid" ), StringDuplicate( s->us_SessionID ) ) == MAP_OK )
												{
												//DEBUG1("[WS]:New values passed to POST %s\n", s->us_SessionID );
												}
										
												if( s->us_UserActionInfo[ 0 ] == 0 )
												{
													int fd = lws_get_socket_fd( fcd->wsc_Wsi );
													char add[ 256 ];
													char rip[ 256 ];
											
													lws_get_peer_addresses( fcd->wsc_Wsi, fd, add, sizeof(add), rip, sizeof(rip) );
													//INFO("[WS]: WEBSOCKET call %s - %s\n", add, rip );
											
													snprintf( s->us_UserActionInfo, sizeof( s->us_UserActionInfo ), "%s / %s", add, rip );
												}
												FRIEND_MUTEX_UNLOCK( &(s->us_Mutex) );
											}
										}
										else
										{
											FRIEND_MUTEX_UNLOCK( &(fcd->wsc_Mutex) );
										}
									}
									
									int i, i1;
									
									//thread
									char **pathParts = wstdata->pathParts;

									BufString *queryrawbs = BufStringNewSize( 2048 );
									
									DEBUG("[WS] Parsing messages\n");
									
									for( i = 7 ; i < r ; i++ )
									{
										i1 = i + 1;
										
										if( jsoneqin( in, &t[i], "requestid") == 0) 
										{
											// threads
											wstdata->requestid = StringDuplicateN(  (char *)(in + t[i1].start), (int)(t[i1].end-t[i1].start) );
											requestid = wstdata->requestid;
											
											if( HashmapPut( http->http_ParsedPostContent, StringDuplicateN(  in + t[ i ].start, t[i].end-t[i].start ), StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start ) ) == MAP_OK )
											{
												//DEBUG1("[WS] New values passed to POST %.*s %.*s\n", t[i].end-t[i].start, (char *)(in + t[i].start), t[i1].end-t[i1].start, (char *)(in + t[i1].start) );
											}
											i++;
										}
										// We got path!
										else if (jsoneqin( in, &t[i], "path") == 0) 
										{
											// this is first path, URI
											
											if( path == NULL )
											{
												// threads
												wstdata->path = StringDuplicateN(  in + t[i1].start,t[i1].end-t[i1].start );
												path = wstdata->path;//in + t[i1].start;
												paths = t[i1].end-t[i1].start;
												
												if( http->http_Uri != NULL )
												{
													http->http_Uri->uri_QueryRaw = StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start );
												}
											
												path[ paths ] = 0;
												int j = 1;
											
												// Don't overwrite first path if it is set!														
												pathParts[ 0 ] = path;
												
												int selpart = 1;
											
												for( j = 1 ; j < paths ; j++ )
												{
													if( path[ j ] == '/' )
													{
														pathParts[ selpart++ ] = &path[ j + 1 ];
														path[ j ] = 0;
													}
												}
												i++;
											}
											else
											{
												// this is path parameter
												if( HashmapPut( http->http_ParsedPostContent, StringDuplicateN(  in + t[ i ].start, t[i].end-t[i].start ), StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start ) ) == MAP_OK )
												{
												}
												i++;
											}
										}
										else if (jsoneqin( in, &t[i], "authid") == 0) 
										{
											authid = in + t[i1].start;
											authids = t[i1].end-t[i1].start;
											
											if( HashmapPut( http->http_ParsedPostContent, StringDuplicateN(  in + t[ i ].start, t[i].end-t[i].start ), StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start ) ) == MAP_OK )
											{
												//DEBUG1("[WS]:New values passed to POST %.*s %.*s\n", t[i].end-t[i].start, in + t[i].start, t[i+1].end-t[i+1].start, in + t[i+1].start );
											}
											
											if( HashmapPut( http->http_ParsedPostContent, StringDuplicateN(  "authid", 6 ), StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start ) ) == MAP_OK )
											{
												//DEBUG1("[WS]:New values passed to POST %s %s\n", "authid", " " );
											}
											i++;
										}
										else
										{
										
										//	  	JSMN_PRIMITIVE = 0,
										//		JSMN_OBJECT = 1,
										//		JSMN_ARRAY = 2,
										//		JSMN_STRING = 3
										//

											if(( i1) < r && t[ i ].type != JSMN_ARRAY )
											{
												if( HashmapPut( http->http_ParsedPostContent, StringDuplicateN( in + t[ i ].start, t[i].end-t[i].start ), StringDuplicateN( in + t[i1].start, t[i1].end-t[i1].start ) ) == MAP_OK )
												{
													//DEBUG1("[WS]:New values passed to POST %.*s %.*s\n", (int)(t[i].end-t[i].start), (char *)(in + t[i].start), (int)(t[i1].end-t[i1].start), (char *)(in + t[i1].start) );
												}
												
												if( t[ i1 ].type == JSMN_ARRAY || t[ i1 ].type == JSMN_OBJECT )
												{
													int z=0;
													if( t[ i1 ].type == JSMN_ARRAY )
													{
														i += t[ i1 ].size;
													}
													i++;
												}
												else
												{
													if( queryrawbs->bs_Size != 0 )
													{
														BufStringAddSize( queryrawbs, "&", 1 );
													}
													BufStringAddSize( queryrawbs, in + t[ i ].start, t[i].end-t[i].start );
													BufStringAddSize( queryrawbs, "=", 1 );
													BufStringAddSize( queryrawbs, in + t[i1].start, t[i1].end-t[i1].start );
													
													i++;
												}
											}
											else
											{
												DEBUG("[WS] Cannot add value: %.*s\n", (int)(t[i].end-t[i].start), (char *)(in + t[i].start) );
												i++;
											}
										}
									} // end of going through json
									
#ifndef INPUT_QUEUE
#if (ENABLE_WEBSOCKETS_THREADS == 1) || ( USE_PTHREAD == 1 )
									// threads
									pthread_t thread;
									memset( &thread, 0, sizeof( pthread_t ) );
									wstdata->http = http;
									//wstdata->wsi = wsi;
									wstdata->fcd = fcd;
									wstdata->queryrawbs = queryrawbs;
									// Multithread mode
#if USE_PTHREAD == 1
									if( pthread_create( &thread, NULL, (void *(*)(void *))WSThread, ( void *)wstdata ) != 0 )
									{
									}
#endif
#if USE_WORKERS == 1
									SystemBase *lsb = (SystemBase *)fcd->wsc_SystemBase;
									
									DEBUG("[WS] Message parsed, sending\n");
									
									if( fcd->wsc_WebsocketsServerClient != NULL )//&& fcd->fcd_WSClient->wsc_ToBeRemoved == FALSE )
									{
										if( http->uri != NULL )
										{
											WorkerManagerRun( lsb->sl_WorkerManager,  WSThread, wstdata, http, http->uri->queryRaw );
										}
										else
										{
											WorkerManagerRun( lsb->sl_WorkerManager,  WSThread, wstdata, http, "ProtocolWebsocket.c: line 1220" );
										}
									}
									else
									{
										releaseWSData( wstdata );
									}
#endif	// USE_WORKERS


#endif // (ENABLE_WEBSOCKETS_THREADS == 1) || ( USE_PTHREAD == 1 )
#else // INPUT_QUEUE
									wstdata->http = http;
									//wstdata->wsi = wsi;
									wstdata->fcd = fcd;
									wstdata->queryrawbs = queryrawbs;
									WSThread( wstdata );
#endif
								}
							}
						}
					
						//
						// events, no need to respoe
						//
						
						else if( strncmp( "event",  in + t[ 6 ].start, t[ 6 ].end-t[ 6 ].start ) == 0 )
						{
							{
								char *requestid = NULL;
								int requestis = 0;
								char *path = NULL;
								int paths = 0;
								char *authid = NULL;
								int authids = 0;
								
								Http *http = HttpNew( );
								if( http != NULL )
								{
									http->http_RequestSource = HTTP_SOURCE_WS;
									http->http_ParsedPostContent = HashmapNew();
									http->http_Uri = UriNew();
									
									UserSession *s = fcd->wsc_UserSession;
									if( s != NULL )
									{
										DEBUG("[WS] Session ptr %p  session %p\n", s, s->us_SessionID );
										if( HashmapPut( http->http_ParsedPostContent, StringDuplicate( "sessionid" ), StringDuplicate( s->us_SessionID ) ) == MAP_OK )
										{
											DEBUG1("[WS] New values passed to POST %s\n", s->us_SessionID );
										}
									
										int i, i1;
										char *pathParts[ 1024 ];		// could be allocated in future
										memset( pathParts, 0, 1024*sizeof(char *) );
									
										BufString *queryrawbs = BufStringNewSize( 2048 );
									
										for( i = 7 ; i < r ; i++ )
										{
											i1 = i + 1;
											if (jsoneqin( in, &t[i], "requestid") == 0) 
											{
												requestid = in + t[i1].start;
												requestis =  t[i1].end-t[i1].start;
											
												if( HashmapPut( http->http_ParsedPostContent, StringDuplicateN(  in + t[ i ].start, t[i].end-t[i].start ), StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start ) ) == MAP_OK )
												{
													//DEBUG1("[WS]:New values passed to POST %.*s %.*s\n", t[i].end-t[i].start, in + t[i].start, t[i+1].end-t[i+1].start, in + t[i+1].start );
												}
												i++;
											}
											else if (jsoneqin( in, &t[i], "path") == 0) 
											{
												// this is first path, URI
											
												if( path == NULL )
												{
													path = in + t[i1].start;
													paths = t[i1].end-t[i1].start;
												
													if( http->http_Uri != NULL )
													{
														http->http_Uri->uri_QueryRaw = StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start );
													}
												
													http->http_RawRequestPath = StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start );
													path[ paths ] = 0;
													int j = 1;
												
													pathParts[ 0 ] = path;
												
													int selpart = 1;
													
													for( j = 1 ; j < paths ; j++ )
													{
														if( path[ j ] == '/' )
														{
															pathParts[ selpart++ ] = &path[ j + 1 ];
															path[ j ] = 0;
														}
													}
													i++;
												}
												else
												{
													// this is path parameter
													if( HashmapPut( http->http_ParsedPostContent, StringDuplicateN(  in + t[ i ].start, t[i].end-t[i].start ), StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start ) ) == MAP_OK )
													{
														//DEBUG1("[WS]:New values passed to POST %.*s %.*s\n", t[i].end-t[i].start, (char *)(in + t[i].start), t[i1].end-t[i1].start, (char *)(in + t[i1].start) );
													}
													i++;
												}
											}
											else if (jsoneqin( in, &t[i], "authId") == 0) 
											{
												authid = in + t[i1].start;
												authids = t[i1].end-t[i1].start;
											
												if( HashmapPut( http->http_ParsedPostContent, StringDuplicateN(  in + t[ i ].start, t[i].end-t[i].start ), StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start ) ) == MAP_OK )
												{
													//DEBUG1("[WS]:New values passed to POST %.*s %.*s\n", t[i].end-t[i].start, in + t[i].start, t[i+1].end-t[i+1].start, in + t[i+1].start );
												}
											
												if( HashmapPut( http->http_ParsedPostContent, StringDuplicateN(  "authid", 6 ), StringDuplicateN(  in + t[i1].start, t[i1].end-t[i1].start ) ) == MAP_OK )
												{

												}
												i++;
											}
											else
											{
												{
													if(( i1) < r && t[ i ].type != JSMN_ARRAY )
													{
														if( HashmapPut( http->http_ParsedPostContent, StringDuplicateN( in + t[ i ].start, t[i].end-t[i].start ), StringDuplicateN( in + t[i1].start, t[i1].end-t[i1].start ) ) == MAP_OK )
														{
															DEBUG1("[WS] New values passed to POST %.*s %.*s\n", (int)(t[i].end-t[i].start), (char *)(in + t[i].start), (int)(t[i1].end-t[i1].start), (char *)(in + t[ i1 ].start) );
														}
													
														if( t[ i1 ].type == JSMN_ARRAY || t[ i1 ].type == JSMN_OBJECT )
														{
															int z=0;
															if( t[ i1 ].type == JSMN_ARRAY )
															{
																DEBUG("[WS] Next  entry is array %d\n", t[ i1 ].size );
																i += t[ i1 ].size;
															}
														
															i++;
															//
															DEBUG1("[WS] current %d skip %d next %d\n", t[ i ].size-1, t[ i1 ].size-1, t[ i1+1 ].size );
														}
														else
														{
															if( queryrawbs->bs_Size != 0 )
															{
																BufStringAddSize( queryrawbs, "&", 1 );
															}
															BufStringAddSize( queryrawbs, in + t[ i ].start, t[i].end-t[i].start );
															BufStringAddSize( queryrawbs, "=", 1 );
															BufStringAddSize( queryrawbs, in + t[i1].start, t[i1].end-t[i1].start );
														
															i++;
														}
													}
													else
													{
														DEBUG("[WS] Cannot add value: %.*s\n", (int)(t[i].end-t[i].start), (char *)(in + t[i].start) );
														i++;
													}
												}
											}
										} // end of going through json
										
										if( strcmp( pathParts[ 0 ], "system.library" ) == 0)
										{
											UserSession *us = (UserSession *)fcd->wsc_UserSession;
											http->http_WSocket = us->us_WSD;
											
											struct timeval start, stop;
											gettimeofday(&start, NULL);
										
											http->http_Content = queryrawbs->bs_Buffer;
											queryrawbs->bs_Buffer = NULL;
											
											http->http_ShutdownPtr = &(SLIB->fcm->fcm_Shutdown);
											
											int respcode = 0;
											
											Http *response = SLIB->SysWebRequest( SLIB, &(pathParts[ 1 ]), &http, fcd->wsc_UserSession, &respcode );
										
											gettimeofday(&stop, NULL);
											double secs = (double)(stop.tv_usec - start.tv_usec) / 1000000 + (double)(stop.tv_sec - start.tv_sec);
										
											DEBUG("[WS] \t\tWS ->SysWebRequest took %f seconds\n", secs );
											
											if( response != NULL )
											{
												char *d = response->http_Content;
												if( d[0] == 'f' && d[1] == 'a' && d[2] == 'i' && d[3] == 'l' )
												{
													char *code = strstr( d, "\"code\":");

													if( code != NULL && 0 == strncmp( code, "\"code\":\"11\"", 11 ) )
													{
														//returnError = -1;
													}
												}
												/*
												if( response->content != NULL && strcmp( response->content, "fail<!--separate-->{\"response\":\"user session not found\"}" )  == 0 )
												{
													//returnError = -1;
												}
												*/
												
												HttpFree( response );
											
												BufStringDelete( queryrawbs );
											}
										}

										if( http != NULL )
										{
											UriFree( http->http_Uri );
											if( http->http_RawRequestPath != NULL )
											{
												FFree( http->http_RawRequestPath );
												http->http_RawRequestPath = NULL;
											}
										}
										HttpFree( http );
									}
								} // session != NULL
								else
								{
									FERROR("[WS] User session is NULL\n");
								}
							}
						}
					}	// type not found
				}	// is JSON_OBJECT
			}
			else	// else if( strncmp( "msg",  in + t[ 2 ].start, 3 ) == 0 )
			{
				FERROR("[WS] Found type %10s \n %10s\n", (char *)(in + t[ 2 ].start), (char *)(in + t[ 1 ].start) );
			}
		}
	}
	else
	{
		char *reqid = NULL;
		reqid = strstr( in, "\"requestid\"" );
		if( reqid != NULL )
		{
			reqid += 13;
			// we want to remove last "
			char *rem = strstr( reqid, "\"" );
			if( rem != NULL )
			{
				*rem = 0;
			}
		}
		
		FERROR("[WS] Failed to parse JSON: %d\n", r);
		unsigned char buf[ 256 ];
		char locmsg[ 256 ];
		int locmsgsize = snprintf( locmsg, sizeof(locmsg), "{\"type\":\"msg\",\"data\":{\"type\":\"error\",\"data\":{\"requestid\":\"%s\"}}}", reqid );
		
		strcpy( (char *)(buf), locmsg );
		
		if( fcd->wsc_UserSession != NULL ) //ORDER IS IMPORTANT
		{
			WebsocketWriteInline( fcd, buf, locmsgsize, LWS_WRITE_TEXT, 3 );
		}
		
		if( in != NULL )
		{
		//	FFree( in );
		}
		
		FERROR("[WS] Object expected\n");
	}
	
	FFree( t );
	
	return 0;
}
