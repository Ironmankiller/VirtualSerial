/*++

Copyright (c) Microsoft Corporation, All Rights Reserved

Module Name:

    Queue.c

Abstract:

    This file implements the I/O queue interface and performs
    the read/write/ioctl operations.

Environment:

    Windows Driver Framework

--*/


#include "internal.h"

#include <stdio.h>
#include <windows.h>
#include <io.h>
#include <synchapi.h>
// Need to link with Ws2_32.lib
#pragma comment (lib, "Ws2_32.lib")

typedef struct sockaddr_in sockaddr_in;
typedef struct WSAData WSAData;
typedef struct sockaddr sockaddr;

VOID DriverEventTrigger(IN WDFQUEUE Queue, IN ULONG events);

DWORD WINAPI ThreadProc(LPVOID lpParam)
{
    NTSTATUS                status = STATUS_SUCCESS;
    PQUEUE_CONTEXT          queueContext = (PQUEUE_CONTEXT)lpParam;
    SOCKET                  ConnectSocket = queueContext->DeviceContext->ConnectSocket;
    BYTE buf[512] = { 0 };
    while (1) {
        int count = 0;
        count = recv(ConnectSocket, buf, 512, 0);

        if (count == 0)break;//被对方关闭
        if (count == SOCKET_ERROR)break;//错误count<0

        status = RingBufferWrite(&queueContext->RingBuffer, buf, count);
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR, "Error at RingBufferWrite");
            break;
        }

        DriverEventTrigger(queueContext->Queue, SERIAL_EV_RXCHAR | SERIAL_EV_RX80FULL);

        Trace(TRACE_LEVEL_INFO, "%s %d", buf, count);
    }
    //结束连接
    closesocket(ConnectSocket);
    queueContext->DeviceContext->ConnectSocket = INVALID_SOCKET;
    WSACleanup();
	return 0;
}

NTSTATUS registry_create_key(WDFKEY parent_key, PUNICODE_STRING key_str,
    WDFKEY* key)
{
    NTSTATUS status;
    WDFKEY new_key;

    status = WdfRegistryCreateKey(parent_key, key_str, KEY_CREATE_SUB_KEY | KEY_WOW64_32KEY,
        REG_OPTION_NON_VOLATILE, NULL, WDF_NO_OBJECT_ATTRIBUTES,
        &new_key);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (key)
        *key = new_key;
    else
        WdfRegistryClose(new_key);

    return status;
}

NTSTATUS
ConnectServer(_In_  PQUEUE_CONTEXT  queueContext) {

    PDEVICE_CONTEXT DeviceContext = queueContext->DeviceContext;
    SOCKET* PConnectSocket        = &DeviceContext->ConnectSocket;
    NTSTATUS status               = STATUS_SUCCESS;

    //----------------------
    // Initialize Winsock.
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != NO_ERROR) {
        Trace(TRACE_LEVEL_ERROR, "WSAStartup failed with error: %ld\n", iResult);
        return -1;
    }

    //----------------------
    // Create a SOCKET for connecting to server
    *PConnectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (*PConnectSocket == INVALID_SOCKET) {
        Trace(TRACE_LEVEL_ERROR, "Error at socket(): %ld\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    //----------------------
    // The sockaddr_in structure specifies the address family,
    // IP address, and port for the socket that is being bound.
    // 
    
    sockaddr_in addrServer;
    addrServer.sin_family = AF_INET;
    addrServer.sin_addr.S_un.S_addr = DeviceContext->IPAddress; 
    addrServer.sin_port = htons((USHORT)DeviceContext->PortNum);

    //----------------------
    // Connect to server.
    // Set timeout in non-blocking mode
    struct timeval timeout;
    fd_set fset;
    int len = sizeof(int);
    int error = -1;

    unsigned long ul = 1;
    ioctlsocket(*PConnectSocket, FIONBIO, &ul);    //set non-blocking mode

    iResult = connect(*PConnectSocket, (SOCKADDR*)&addrServer, sizeof(addrServer));
    if (iResult == SOCKET_ERROR) {
        FD_ZERO(&fset);
        FD_SET(*PConnectSocket, &fset);
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000;
        if (select(0, NULL, &fset, NULL, &timeout) > 0) {
            getsockopt(*PConnectSocket, SOL_SOCKET, SO_ERROR, (char*)&error, &len);
        }
    }
    if (error !=0 ) {
        closesocket(*PConnectSocket);
        *PConnectSocket = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }
    ul = 0;
    ioctlsocket(*PConnectSocket, FIONBIO, &ul); //set blocking mode

    //----------------------
    // Create a thread blocking on recv()
    DWORD dwThread;
    HANDLE hThread = CreateThread(NULL, 0, ThreadProc, queueContext, 0, &dwThread);
    if (hThread == NULL)
    {
        Trace(TRACE_LEVEL_ERROR, "Thread Creat Failed");
        closesocket(*PConnectSocket);
        *PConnectSocket = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }
    CloseHandle(hThread);

    return status;
}
   
NTSTATUS
QueueCreate(
    _In_  PDEVICE_CONTEXT   DeviceContext
    )
{
    NTSTATUS                status;
    WDFDEVICE               device = DeviceContext->Device;
    WDF_IO_QUEUE_CONFIG     queueConfig;
    WDF_OBJECT_ATTRIBUTES   queueAttributes;
    WDFQUEUE                queue;
    PQUEUE_CONTEXT          queueContext;


    //
    // Create the default queue
    //

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
                            &queueConfig,
                            WdfIoQueueDispatchParallel);

    queueConfig.EvtIoRead           = EvtIoRead;
    queueConfig.EvtIoWrite          = EvtIoWrite;
    queueConfig.EvtIoDeviceControl  = EvtIoDeviceControl;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
                            &queueAttributes,
                            QUEUE_CONTEXT);

    status = WdfIoQueueCreate(
                            device,
                            &queueConfig,
                            &queueAttributes,
                            &queue);

    if( !NT_SUCCESS(status) ) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfIoQueueCreate failed 0x%x", status);
        return status;
    }

    queueContext = GetQueueContext(queue);
    queueContext->Queue = queue;
    queueContext->DeviceContext = DeviceContext;

    //
    // Create a manual queue to hold pending read requests. By keeping
    // them in the queue, framework takes care of cancelling them if the app
    // exits
    //

    WDF_IO_QUEUE_CONFIG_INIT(
                            &queueConfig,
                            WdfIoQueueDispatchManual);

    status = WdfIoQueueCreate(
                            device,
                            &queueConfig,
                            WDF_NO_OBJECT_ATTRIBUTES,
                            &queue);

    if( !NT_SUCCESS(status) ) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfIoQueueCreate manual queue failed 0x%x", status);
        return status;
    }

    queueContext->ReadQueue = queue;

    //
    // Create another manual queue to hold pending IOCTL_SERIAL_WAIT_ON_MASK
    //

    WDF_IO_QUEUE_CONFIG_INIT(
                            &queueConfig,
                            WdfIoQueueDispatchManual);

    status = WdfIoQueueCreate(
                            device,
                            &queueConfig,
                            WDF_NO_OBJECT_ATTRIBUTES,
                            &queue);

    if( !NT_SUCCESS(status) ) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfIoQueueCreate manual queue failed 0x%x", status);
        return status;
    }

    queueContext->WaitMaskQueue = queue;

    RingBufferInitialize(&queueContext->RingBuffer,
                            queueContext->Buffer,
                            sizeof(queueContext->Buffer));

    return status;
}


NTSTATUS
RequestCopyFromBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             SourceBuffer,
    _In_  size_t            NumBytesToCopyFrom
    )
{
    NTSTATUS                status;
    WDFMEMORY               memory;

    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if( !NT_SUCCESS(status) ) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfRequestRetrieveOutputMemory failed 0x%x", status);
        return status;
    }

    status = WdfMemoryCopyFromBuffer(memory, 0,
                            SourceBuffer, NumBytesToCopyFrom);
    if( !NT_SUCCESS(status) ) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfMemoryCopyFromBuffer failed 0x%x", status);
        return status;
    }

    WdfRequestSetInformation(Request, NumBytesToCopyFrom);
    return status;
}


NTSTATUS
RequestCopyToBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             DestinationBuffer,
    _In_  size_t            NumBytesToCopyTo
    )
{
    NTSTATUS                status;
    WDFMEMORY               memory;

    status = WdfRequestRetrieveInputMemory(Request, &memory);
    if( !NT_SUCCESS(status) ) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfRequestRetrieveInputMemory failed 0x%x", status);
        return status;
    }

    status = WdfMemoryCopyToBuffer(memory, 0,
                            DestinationBuffer, NumBytesToCopyTo);
    if( !NT_SUCCESS(status) ) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfMemoryCopyToBuffer failed 0x%x", status);
        return status;
    }

    WdfRequestSetInformation(Request, NumBytesToCopyTo);
    return status;
}


void PrintIoControlCode(ULONG code)
{
	switch (code)
	{
	case IOCTL_SERIAL_CLEAR_STATS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_CLEAR_STATS\n");
		break;
	case IOCTL_SERIAL_CLR_DTR:
        Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_CLR_DTR\n");
		break;
	case IOCTL_SERIAL_CLR_RTS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_CLR_RTS\n");
		break;
	case IOCTL_SERIAL_CONFIG_SIZE:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_CONFIG_SIZE\n");
		break;
	case IOCTL_SERIAL_GET_BAUD_RATE:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_BAUD_RATE\n");
		break;
	case IOCTL_SERIAL_GET_CHARS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_CHARS\n");
		break;
	case IOCTL_SERIAL_GET_COMMSTATUS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_COMMSTATUS\n");
		break;
	case IOCTL_SERIAL_GET_DTRRTS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_DTRRTS\n");
		break;
	case IOCTL_SERIAL_GET_HANDFLOW:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_HANDFLOW\n");
		break;
	case IOCTL_SERIAL_GET_LINE_CONTROL:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_LINE_CONTROL\n");
		break;
	case IOCTL_SERIAL_GET_MODEM_CONTROL:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_MODEM_CONTROL\n");
		break;
	case IOCTL_SERIAL_GET_MODEMSTATUS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_MODEMSTATUS\n");
		break;
	case IOCTL_SERIAL_GET_PROPERTIES:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_PROPERTIES\n");
		break;
	case IOCTL_SERIAL_GET_STATS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_STATS\n");
		break;
	case IOCTL_SERIAL_GET_TIMEOUTS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_TIMEOUTS\n");
		break;
	case IOCTL_SERIAL_GET_WAIT_MASK:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_WAIT_MASK\n");
		break;
	case IOCTL_SERIAL_IMMEDIATE_CHAR:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_IMMEDIATE_CHAR\n");
		break;
	case IOCTL_SERIAL_LSRMST_INSERT:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_LSRMST_INSERT\n");
		break;
	case IOCTL_SERIAL_PURGE:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_PURGE\n");
		break;
	case IOCTL_SERIAL_RESET_DEVICE:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_RESET_DEVICE\n");
		break;
	case IOCTL_SERIAL_SET_BAUD_RATE:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_BAUD_RATE\n");
		break;
	case IOCTL_SERIAL_SET_BREAK_OFF:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_BREAK_OFF\n");
		break;
	case IOCTL_SERIAL_SET_BREAK_ON:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_BREAK_ON\n");
		break;
	case IOCTL_SERIAL_SET_CHARS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_CHARS\n");
		break;
	case IOCTL_SERIAL_SET_DTR:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_DTR\n");
		break;
	case IOCTL_SERIAL_SET_FIFO_CONTROL:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_FIFO_CONTROL\n");
		break;
	case IOCTL_SERIAL_SET_HANDFLOW:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_HANDFLOW\n");
		break;
	case IOCTL_SERIAL_SET_LINE_CONTROL:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_LINE_CONTROL\n");
		break;
	case IOCTL_SERIAL_SET_MODEM_CONTROL:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_MODEM_CONTROL\n");
		break;
	case IOCTL_SERIAL_SET_QUEUE_SIZE:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_QUEUE_SIZE\n");
		break;
	case IOCTL_SERIAL_SET_RTS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_RTS\n");
		break;
	case IOCTL_SERIAL_SET_TIMEOUTS:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_TIMEOUTS\n");
		break;
	case IOCTL_SERIAL_SET_WAIT_MASK:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_WAIT_MASK\n");
		break;
	case IOCTL_SERIAL_SET_XOFF:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_XOFF\n");
		break;
	case IOCTL_SERIAL_SET_XON:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_XON\n");
		break;
	case IOCTL_SERIAL_WAIT_ON_MASK:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_WAIT_ON_MASK\n");
		break;
	case IOCTL_SERIAL_XOFF_COUNTER:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_XOFF_COUNTER\n");
		break;
    case IOCTL_SERIAL_SET_IPADDR:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_IPADDR\n");
		break;
	case IOCTL_SERIAL_GET_IPADDR:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_IPADDR\n");
		break;
	case IOCTL_SERIAL_SET_PORTNUM:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_SET_PORTNUM\n");
		break;
	case IOCTL_SERIAL_GET_PORTNUM:
		Trace(TRACE_LEVEL_INFO, "IOCTL code: IOCTL_SERIAL_GET_PORTNUM\n");
		break;
	default:
        Trace(TRACE_LEVEL_ERROR, "IOCTL code: unkown\n");
	}
}

static BOOL isConfigureNetwork(_In_  ULONG  IoControlCode) {
    return IoControlCode == IOCTL_SERIAL_SET_IPADDR ||
        IoControlCode == IOCTL_SERIAL_GET_IPADDR ||
        IoControlCode == IOCTL_SERIAL_SET_PORTNUM ||
        IoControlCode == IOCTL_SERIAL_GET_PORTNUM;
}

VOID
EvtIoDeviceControl(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            OutputBufferLength,
    _In_  size_t            InputBufferLength,
    _In_  ULONG             IoControlCode
    )
{
    NTSTATUS                status = STATUS_SUCCESS;
    PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
    PDEVICE_CONTEXT         deviceContext = queueContext->DeviceContext;
    UNREFERENCED_PARAMETER  (OutputBufferLength);
    UNREFERENCED_PARAMETER  (InputBufferLength);

    PrintIoControlCode(IoControlCode);
    
    if (deviceContext->ConnectSocket == INVALID_SOCKET && !isConfigureNetwork(IoControlCode)) {
		int tryTimes = 1;
		do {
			Trace(TRACE_LEVEL_INFO, "Try to connect server %d...", tryTimes);
			status = ConnectServer(queueContext);
			if (NT_SUCCESS(status)) {
				Trace(TRACE_LEVEL_INFO, "Connect server success.");
				break;
			}
		} while (tryTimes++ != 3);   // retry 3 times
        if (!NT_SUCCESS(status)) {
            WdfRequestCompleteWithInformation(Request, status, 0);
            return;
        }
    }


    switch (IoControlCode)
    {

    case IOCTL_SERIAL_SET_BAUD_RATE:
    {
        //
        // This is a driver for a virtual serial port. Since there is no
        // actual hardware, we just store the baud rate and don't do
        // anything with it.
        //
        SERIAL_BAUD_RATE baudRateBuffer = {0};

        status = RequestCopyToBuffer(Request,
                            &baudRateBuffer,
                            sizeof(baudRateBuffer));

        if( NT_SUCCESS(status) ) {
            SetBaudRate(deviceContext, baudRateBuffer.BaudRate);
        };
        break;
    }

    case IOCTL_SERIAL_GET_BAUD_RATE:
    {
        SERIAL_BAUD_RATE baudRateBuffer = {0};

        baudRateBuffer.BaudRate = GetBaudRate(deviceContext);

        status = RequestCopyFromBuffer(Request,
                            &baudRateBuffer,
                            sizeof(baudRateBuffer));
        break;
    }

	case IOCTL_SERIAL_SET_RTS:
	{
        deviceContext->RTSstate = 1;
		break;
	}

	case IOCTL_SERIAL_CLR_RTS:
	{
        deviceContext->RTSstate = 0;
		break;
	}

	case IOCTL_SERIAL_SET_DTR:
	{
        deviceContext->DTRstate = 1;
		break;
	}

	case IOCTL_SERIAL_CLR_DTR:
	{
        deviceContext->DTRstate = 0;
		break;
	}

    case IOCTL_SERIAL_GET_DTRRTS:
    {
        ULONG ModemControl;

        ModemControl = deviceContext->DTRstate + (deviceContext->RTSstate << 1);

		status = RequestCopyFromBuffer(Request,
			&ModemControl,
			sizeof(ULONG));

        break;
    }

    case IOCTL_SERIAL_GET_MODEMSTATUS:
    {  
		ULONG Cts, Dsr, Dcd;
        ULONG ModemStatus;

		Cts = 1; //对方传送请求				//pdx->pOther->RTSstate;

		Dsr = 1; //对方数据终端是否准备好	//pdx->pOther->DTRstate;

		Dcd = 1; //对方设备是否打开	//pdx->pOther->IsOpen;

        ModemStatus = (Cts ? SERIAL_CTS_STATE : 0) | (Dsr ? SERIAL_DSR_STATE : 0) |
            (Dcd ? SERIAL_DCD_STATE : 0);

		status = RequestCopyFromBuffer(Request,
			&ModemStatus,
			sizeof(ULONG));
        break;
    }

	case IOCTL_SERIAL_GET_TIMEOUTS:
	{
		SERIAL_TIMEOUTS timeoutValues = { 0 };

		GetTimeouts(deviceContext, &timeoutValues);

		status = RequestCopyFromBuffer(Request,
			(void*)&timeoutValues,
			sizeof(timeoutValues));
		break;
	}

	case IOCTL_SERIAL_SET_TIMEOUTS:
	{
		SERIAL_TIMEOUTS timeoutValues = { 0 };

		status = RequestCopyToBuffer(Request,
			(void*)&timeoutValues,
			sizeof(timeoutValues));

		if (NT_SUCCESS(status))
		{
			if ((timeoutValues.ReadIntervalTimeout == MAXULONG) &&
				(timeoutValues.ReadTotalTimeoutMultiplier == MAXULONG) &&
				(timeoutValues.ReadTotalTimeoutConstant == MAXULONG))
			{
				status = STATUS_INVALID_PARAMETER;
			}
		}

		if (NT_SUCCESS(status)) {
			SetTimeouts(deviceContext, timeoutValues);
		}

		break;
	}


    case IOCTL_SERIAL_SET_MODEM_CONTROL:
    {
        //
        // This is a driver for a virtual serial port. Since there is no
        // actual hardware, we just store the modem control register
        // configuration and don't do anything with it.
        //
        ULONG *modemControlRegister = GetModemControlRegisterPtr(deviceContext);

        ASSERT(modemControlRegister);

        status = RequestCopyToBuffer(Request,
                            modemControlRegister,
                            sizeof(ULONG));
        break;
    }

    case IOCTL_SERIAL_GET_MODEM_CONTROL:
    {
        ULONG *modemControlRegister = GetModemControlRegisterPtr(deviceContext);

        ASSERT(modemControlRegister);

        status = RequestCopyFromBuffer(Request,
                            modemControlRegister,
                            sizeof(ULONG));
        break;
    }

    case IOCTL_SERIAL_SET_FIFO_CONTROL:
    {
        //
        // This is a driver for a virtual serial port. Since there is no
        // actual hardware, we just store the FIFO control register
        // configuration and don't do anything with it.
        //
        ULONG *fifoControlRegister = GetFifoControlRegisterPtr(deviceContext);

        ASSERT(fifoControlRegister);

        status = RequestCopyToBuffer(Request,
                            fifoControlRegister,
                            sizeof(ULONG));
        break;
    }

    case IOCTL_SERIAL_GET_LINE_CONTROL:
    {
        status = QueueProcessGetLineControl(
                            queueContext,
                            Request);
        break;
    }


    case IOCTL_SERIAL_SET_LINE_CONTROL:
    {
        status = QueueProcessSetLineControl(
                            queueContext,
                            Request);
        break;
    }

    
    case IOCTL_SERIAL_WAIT_ON_MASK:
    {
        //
        // NOTE: A wait-on-mask request should not be completed until either:
        //  1) A wait event occurs; or
        //  2) A set-wait-mask request is received
        //
        // This is a driver for a virtual serial port. Since there is no
        // actual hardware, we complete the request with some failure code.
        //
        WDFREQUEST savedRequest;

        status = WdfIoQueueRetrieveNextRequest(
                            queueContext->WaitMaskQueue,
                            &savedRequest);

        Trace(TRACE_LEVEL_INFO, "getWaitQueue: %d", NT_SUCCESS(status));

        if (NT_SUCCESS(status)) {
            WdfRequestComplete(savedRequest,
                            STATUS_UNSUCCESSFUL);
        }
        else if (((queueContext->EventMask) & (queueContext->EventHappened)) != 0) {
            // Some events happened
            ULONG happened = queueContext->EventMask & queueContext->EventHappened;

            Trace(TRACE_LEVEL_INFO, "Event happen: %d", happened);

            status = RequestCopyFromBuffer(Request,
                &happened,
                sizeof(ULONG));

            queueContext->EventHappened = 0;

            WdfRequestComplete(Request, status);
        }
        else {
            //
            // Keep the request in a manual queue and the framework will take
            // care of cancelling them when the app exits
            //
            Trace(TRACE_LEVEL_INFO, "Go to wait mask queue");

            status = WdfRequestForwardToIoQueue(
                Request,
                queueContext->WaitMaskQueue);

            if (!NT_SUCCESS(status)) {
                Trace(TRACE_LEVEL_ERROR,
                    "Error: WdfRequestForwardToIoQueue failed 0x%x", status);
                WdfRequestComplete(Request, status);
            }
        }

        //
        // Instead of "break", use "return" to prevent the current request
        // from being completed.
        //
        return;
    }

    case IOCTL_SERIAL_SET_WAIT_MASK:
    {

        status = RequestCopyToBuffer(Request,
            &queueContext->EventMask,
            sizeof(queueContext->EventMask));

        if (NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_INFO, "Set Mask OK %x", queueContext->EventMask);
        }
        //
        // NOTE: If a wait-on-mask request is already pending when set-wait-mask
        // request is processed, the pending wait-on-event request is completed
        // with STATUS_SUCCESS and the output wait event mask is set to zero.
        //
        WDFREQUEST savedRequest;

        //对以前pending的完成例程，进行完成
        status = WdfIoQueueRetrieveNextRequest(
                            queueContext->WaitMaskQueue,
                            &savedRequest);

        if (NT_SUCCESS(status)) {

            ULONG eventMask = 0;

            status = RequestCopyFromBuffer(
                            savedRequest,
                            &eventMask,
                            sizeof(eventMask));

            WdfRequestComplete(savedRequest, status);
        }

        //
        // NOTE: The application expects STATUS_SUCCESS for these IOCTLs.
        //
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_SERIAL_GET_WAIT_MASK:
    {
        status = RequestCopyFromBuffer(Request,
            (void*)&queueContext->EventMask,
            sizeof(queueContext->EventMask));
        break;
    }

    case IOCTL_SERIAL_GET_COMMSTATUS:
    {
        SERIAL_STATUS  serialStatus = {0};

        size_t availableData = 0;

        RingBufferGetAvailableData(
            &queueContext->RingBuffer,
            &availableData);

        ULONG InputLen = availableData;

        Trace(TRACE_LEVEL_INFO, "availableData: %d", (ULONG)availableData);

        serialStatus.AmountInInQueue = InputLen;

        serialStatus.AmountInOutQueue = 0;

        status = RequestCopyFromBuffer(Request,
            (void*)&serialStatus,
            sizeof(serialStatus));

        break;
    }

    case IOCTL_SERIAL_GET_CHARS:
    {
        SERIAL_CHARS serialChars = { 0 };

        status = RequestCopyFromBuffer(Request,
            (void*)&serialChars,
            sizeof(serialChars));

        break;
    }

    case IOCTL_SERIAL_SET_IPADDR:
    {
        WDFKEY subkey = { 0 };
		DECLARE_CONST_UNICODE_STRING(ipAddrName, REG_VALUENAME_IPADDR);

        ULONG tempIPAddr = { 0 };
		status = RequestCopyToBuffer(Request,
			(PVOID)&tempIPAddr,
			sizeof(ULONG));
        
		// open the ServiceName subkey of the device's hardware key for read/write access.
		status = WdfDeviceOpenRegistryKey(
			deviceContext->Device,
			PLUGPLAY_REGKEY_DEVICE | WDF_REGKEY_DEVICE_SUBKEY,
			KEY_READ | KEY_SET_VALUE,
			WDF_NO_OBJECT_ATTRIBUTES,
			&subkey);
		if (!NT_SUCCESS(status)) {
			Trace(TRACE_LEVEL_ERROR,
				"Error: Failed to retrieve device hardware service name key");
            break;
		}
        // Update Registry subkey and Device Context
		status = WdfRegistryAssignULong(
			subkey,
			&ipAddrName,
            tempIPAddr);
		if (!NT_SUCCESS(status)) {
			Trace(TRACE_LEVEL_ERROR,
				"Error: Failed to assign ipAddrName");
            break;
		}
        deviceContext->IPAddress = tempIPAddr;
        closesocket(deviceContext->ConnectSocket);
        deviceContext->ConnectSocket = INVALID_SOCKET;
        WSACleanup();
        break;
    }

    case IOCTL_SERIAL_GET_IPADDR:
    {
        status = RequestCopyFromBuffer(Request,
            (PVOID)&deviceContext->IPAddress,
            sizeof(ULONG));
        break;
    }

    case IOCTL_SERIAL_SET_PORTNUM:
    {
        WDFKEY subkey = { 0 };
        DECLARE_CONST_UNICODE_STRING(portNumName, REG_VALUENAME_PORTNUM);

        ULONG tempPortNum = { 0 };
        status = RequestCopyToBuffer(Request,
            (PVOID)&tempPortNum,
            sizeof(ULONG));

        // open the ServiceName subkey of the device's hardware key for read/write access.
        status = WdfDeviceOpenRegistryKey(
            deviceContext->Device,
            PLUGPLAY_REGKEY_DEVICE | WDF_REGKEY_DEVICE_SUBKEY,
            KEY_READ | KEY_SET_VALUE,
            WDF_NO_OBJECT_ATTRIBUTES,
            &subkey);
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR,
                "Error: Failed to retrieve device hardware service name key");
            break;
        }
        // Update Registry subkey and Device Context
        status = WdfRegistryAssignULong(
            subkey,
            &portNumName,
            tempPortNum);
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR,
                "Error: Failed to assign PortNum");
            break;
        }
        deviceContext->PortNum = tempPortNum;
        closesocket(deviceContext->ConnectSocket);
        deviceContext->ConnectSocket = INVALID_SOCKET;
        WSACleanup();
        break;
    }

    case IOCTL_SERIAL_GET_PORTNUM:
    {
        status = RequestCopyFromBuffer(Request,
            (PVOID)&deviceContext->PortNum,
            sizeof(ULONG));
        break;
    }

	case IOCTL_SERIAL_SET_HANDFLOW:
    case IOCTL_SERIAL_PURGE:
    case IOCTL_SERIAL_SET_QUEUE_SIZE:
    case IOCTL_SERIAL_SET_XON:
    case IOCTL_SERIAL_SET_XOFF:
    case IOCTL_SERIAL_SET_CHARS:
    case IOCTL_SERIAL_GET_HANDFLOW:
    case IOCTL_SERIAL_RESET_DEVICE:
    {
        //
        // NOTE: The application expects STATUS_SUCCESS for these IOCTLs.
        //
        status = STATUS_SUCCESS;
        break;
    }

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    //
    // complete the request
    //
    WdfRequestComplete(Request, status);
}

VOID DriverEventTrigger(IN WDFQUEUE Queue, IN ULONG events)
{
    KdPrint(("DriverEventTrigger\n"));

    NTSTATUS status = STATUS_SUCCESS;
    PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
    WDFREQUEST              savedRequest;

    queueContext->EventHappened |= events;

    events &= queueContext->EventMask;

    for (; ; ) {   // 只会有一个等待的事件

        status = WdfIoQueueRetrieveNextRequest(
            queueContext->WaitMaskQueue,
            &savedRequest);

        if (!NT_SUCCESS(status)) {
            break;
        }

        KdPrint(("Take out a wait mask request\n"));

        status = WdfRequestForwardToIoQueue(
            savedRequest,
            Queue);

        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR,
                "Error: WdfRequestForwardToIoQueue failed 0x%x", status);
            WdfRequestComplete(savedRequest, status);
        }
    }
}

VOID
EvtIoWrite(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            Length
    )
{
    NTSTATUS                status;
    PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
    WDFMEMORY               memory;
    size_t                  availableData = 0;

    Trace(TRACE_LEVEL_INFO,
            "EvtIoWrite 0x%p Length %d", Request, (ULONG)Length);

    status = WdfRequestRetrieveInputMemory(Request, &memory);
    if( !NT_SUCCESS(status) ) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfRequestRetrieveInputMemory failed 0x%x", status);
        return;
    }

    
    size_t sendLength = 0;

	status = QueueProcessWriteBytes(
		queueContext,
		(PUCHAR)WdfMemoryGetBuffer(memory, NULL),
		Length, &sendLength);

	if (!NT_SUCCESS(status)) {
		WdfRequestCompleteWithInformation(Request,
			STATUS_INVALID_NETWORK_RESPONSE,
            sendLength);
		return;
	}

    WdfRequestCompleteWithInformation(Request, status, Length);
}


VOID
EvtIoRead(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            Length
    )
{
    NTSTATUS                status;
    PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
    PDEVICE_CONTEXT         DeviceContext = queueContext->DeviceContext;
    WDFMEMORY               memory;
    size_t                  bytesCopied = 0;

    Trace(TRACE_LEVEL_INFO,
            "EvtIoRead 0x%p Length %d", Request, (ULONG)Length);

    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if( !NT_SUCCESS(status) ) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfRequestRetrieveOutputMemory failed 0x%x", status);
        WdfRequestComplete(Request, status);
        return;
    }

    status = RingBufferRead(&queueContext->RingBuffer,
                            (BYTE*)WdfMemoryGetBuffer(memory, NULL),
                            Length,
                            &bytesCopied);
    if( !NT_SUCCESS(status) ) {
        WdfRequestComplete(Request, status);
        return;
    }

    if (bytesCopied > 0) {
        //
        // Data was read from buffer succesfully
        //
		Trace(TRACE_LEVEL_INFO,
			"Read success Length %d", (ULONG)bytesCopied);
        WdfRequestCompleteWithInformation(Request, status, bytesCopied);
        return;
    }
    else {
        //
        // No data to read. Queue the request for later processing.
        //
        status = WdfRequestForwardToIoQueue(Request,
                            queueContext->ReadQueue);
        if( !NT_SUCCESS(status) ) {
            Trace(TRACE_LEVEL_ERROR,
                "Error: WdfRequestForwardToIoQueue failed 0x%x", status);
            WdfRequestComplete(Request, status);
        }
    }
}


NTSTATUS
QueueProcessWriteBytes(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_reads_bytes_(Length)
          PUCHAR            buf,
    _In_  size_t            Length,
    _Out_ size_t*           sendLength
    )
{
    NTSTATUS                status = STATUS_SUCCESS;
    PDEVICE_CONTEXT         DeviceContext = QueueContext->DeviceContext;
    //
	// Process input
	//
	int count = Length;
	int sendCount = 0, currentPosition = 0;
	int tryTimes = 1;
	do {
		while (count > 0 && (sendCount = send(DeviceContext->ConnectSocket, buf + currentPosition, count, 0)) != SOCKET_ERROR)
		{
			count -= sendCount;
			currentPosition += sendCount;
		}
		if (sendCount == SOCKET_ERROR) {
			Trace(TRACE_LEVEL_INFO, "Try to connect server %d...", tryTimes);

			status = ConnectServer(QueueContext);
			if (NT_SUCCESS(status)) {
				Trace(TRACE_LEVEL_INFO, "Connect server success.");
				tryTimes = 0;
			}
		}
	} while (sendCount != 0 && tryTimes++ != 3);   // retry 3 times

    *sendLength = currentPosition;

	if (sendCount == SOCKET_ERROR) {
		Trace(TRACE_LEVEL_ERROR, "Network Error");
		return STATUS_INVALID_NETWORK_RESPONSE;
	}
    return status;
}


NTSTATUS
QueueProcessGetLineControl(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
{
    NTSTATUS                status;
    PDEVICE_CONTEXT         deviceContext;
    SERIAL_LINE_CONTROL     lineControl = {0};
    ULONG                   lineControlSnapshot;
    ULONG                   *lineControlRegister;

    deviceContext = QueueContext->DeviceContext;
    lineControlRegister = GetLineControlRegisterPtr(deviceContext);

    ASSERT(lineControlRegister);

    //
    // Take a snapshot of the line control register variable
    //
    lineControlSnapshot = *lineControlRegister;

    //
    // Decode the word length
    //
    if ((lineControlSnapshot & SERIAL_DATA_MASK) == SERIAL_5_DATA)
    {
        lineControl.WordLength = 5;
    }
    else if ((lineControlSnapshot & SERIAL_DATA_MASK) == SERIAL_6_DATA)
    {
        lineControl.WordLength = 6;
    }
    else if ((lineControlSnapshot & SERIAL_DATA_MASK) == SERIAL_7_DATA)
    {
        lineControl.WordLength = 7;
    }
    else if ((lineControlSnapshot & SERIAL_DATA_MASK) == SERIAL_8_DATA)
    {
        lineControl.WordLength = 8;
    }

    //
    // Decode the parity
    //
    if ((lineControlSnapshot & SERIAL_PARITY_MASK) == SERIAL_NONE_PARITY)
    {
        lineControl.Parity = NO_PARITY;
    }
    else if ((lineControlSnapshot & SERIAL_PARITY_MASK) == SERIAL_ODD_PARITY)
    {
        lineControl.Parity = ODD_PARITY;
    }
    else if ((lineControlSnapshot & SERIAL_PARITY_MASK) == SERIAL_EVEN_PARITY)
    {
        lineControl.Parity = EVEN_PARITY;
    }
    else if ((lineControlSnapshot & SERIAL_PARITY_MASK) == SERIAL_MARK_PARITY)
    {
        lineControl.Parity = MARK_PARITY;
    }
    else if ((lineControlSnapshot & SERIAL_PARITY_MASK) == SERIAL_SPACE_PARITY)
    {
        lineControl.Parity = SPACE_PARITY;
    }

    //
    // Decode the length of the stop bit
    //
    if (lineControlSnapshot & SERIAL_2_STOP)
    {
        if (lineControl.WordLength == 5)
        {
            lineControl.StopBits = STOP_BITS_1_5;
        }
        else
        {
            lineControl.StopBits = STOP_BITS_2;
        }
    }
    else
    {
        lineControl.StopBits = STOP_BIT_1;
    }

    //
    // Copy the information that was decoded to the caller's buffer
    //
    status = RequestCopyFromBuffer(Request,
                        (void*) &lineControl,
                        sizeof(lineControl));
    return status;
}


NTSTATUS
QueueProcessSetLineControl(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
{
    NTSTATUS                status;
    PDEVICE_CONTEXT         deviceContext;
    SERIAL_LINE_CONTROL     lineControl = {0};
    ULONG                   *lineControlRegister;
    UCHAR                   lineControlData = 0;
    UCHAR                   lineControlStop = 0;
    UCHAR                   lineControlParity = 0;
    ULONG                   lineControlSnapshot;
    ULONG                   lineControlNew;
    ULONG                   lineControlPrevious;
    ULONG                   i;

    deviceContext = QueueContext->DeviceContext;
    lineControlRegister = GetLineControlRegisterPtr(deviceContext);

    ASSERT(lineControlRegister);

    //
    // This is a driver for a virtual serial port. Since there is no
    // actual hardware, we just store the line control register
    // configuration and don't do anything with it.
    //
    status = RequestCopyToBuffer(Request,
                        (void*) &lineControl,
                        sizeof(lineControl));

    //
    // Bits 0 and 1 of the line control register
    //
    if( NT_SUCCESS(status) )
    {
        switch (lineControl.WordLength)
        {
        case 5:
            lineControlData = SERIAL_5_DATA;
            SetValidDataMask(deviceContext, 0x1f);
            break;

        case 6:
            lineControlData = SERIAL_6_DATA;
            SetValidDataMask(deviceContext, 0x3f);
            break;

        case 7:
            lineControlData = SERIAL_7_DATA;
            SetValidDataMask(deviceContext, 0x7f);
            break;

        case 8:
            lineControlData = SERIAL_8_DATA;
            SetValidDataMask(deviceContext, 0xff);
            break;

        default:
            status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

    //
    // Bit 2 of the line control register
    //
    if( NT_SUCCESS(status) )
    {
        switch (lineControl.StopBits)
        {
        case STOP_BIT_1:
            lineControlStop = SERIAL_1_STOP;
            break;

        case STOP_BITS_1_5:
            if (lineControlData != SERIAL_5_DATA)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            lineControlStop = SERIAL_1_5_STOP;
            break;

        case STOP_BITS_2:
            if (lineControlData == SERIAL_5_DATA)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            lineControlStop = SERIAL_2_STOP;
            break;

        default:
            status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

    //
    // Bits 3, 4 and 5 of the line control register
    //
    if( NT_SUCCESS(status) )
    {
        switch (lineControl.Parity)
        {
        case NO_PARITY:
            lineControlParity = SERIAL_NONE_PARITY;
            break;

        case EVEN_PARITY:
            lineControlParity = SERIAL_EVEN_PARITY;
            break;

        case ODD_PARITY:
            lineControlParity = SERIAL_ODD_PARITY;
            break;

        case SPACE_PARITY:
            lineControlParity = SERIAL_SPACE_PARITY;
            break;

        case MARK_PARITY:
            lineControlParity = SERIAL_MARK_PARITY;
            break;

        default:
            status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

    //
    // Update our line control register variable atomically
    //
    i=0;
    do {
        i++;
        if ((i & 0xf) == 0) {
            //
            // We've been spinning in a loop for a while trying to
            // update the line control register variable atomically.
            // Yield the CPU for other threads for a while.
            //
#ifdef _KERNEL_MODE
            LARGE_INTEGER   interval;
            interval.QuadPart = 0;
            KeDelayExecutionThread(UserMode, FALSE, &interval);
#else
            SwitchToThread();
#endif
        }

        lineControlSnapshot = *lineControlRegister;

        lineControlNew = (lineControlSnapshot & SERIAL_LCR_BREAK) |
                        (lineControlData | lineControlParity | lineControlStop);

        lineControlPrevious = InterlockedCompareExchange(
                      (LONG *) lineControlRegister,
                       lineControlNew,
                       lineControlSnapshot);

    } while (lineControlPrevious != lineControlSnapshot);

    return status;
}
