/*
 * p3pair - keeps a Bluetooth HID gamepad paired without any user interaction.
 *
 * While the target device is not connected, runs a Bluetooth inquiry. When the
 * device shows up (i.e. it is in pairing mode), any stale pairing is removed,
 * the device is paired (PIN / confirmation answered automatically) and its HID
 * service is enabled, so Windows installs the driver silently instead of
 * showing the "device is trying to connect" balloon and the hardware wizard.
 *
 * Windows Vista SP2 / 7 or newer, Microsoft Bluetooth stack.
 * Build: cl /nologo /W3 /O2 p3pair.c   (links Bthprops.lib via #pragma)
 *
 */
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <bluetoothapis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _MSC_VER
#pragma comment(lib, "Bthprops.lib")
#endif

/* HumanInterfaceDeviceServiceClass_UUID, defined locally to avoid initguid.h ordering issues */
static GUID HidServiceUuid ={ 0x00001124, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };

#ifndef BLUETOOTH_SERVICE_ENABLE
#define BLUETOOTH_SERVICE_ENABLE 0x01 /* missing from MinGW headers */
#endif

#define GRACE_MS 20000 /* after pairing, wait this long for the HID link before retrying */

static BLUETOOTH_ADDRESS g_target;
static char g_pin[BTH_MAX_PIN_SIZE + 1] = "0000";
static UCHAR g_scanMult = 4; /* inquiry length in units of 1.28 s */
static HBLUETOOTH_AUTHENTICATION_REGISTRATION g_authReg = NULL;

static void logmsg(const char *fmt, ...)
{
	SYSTEMTIME t;
	va_list ap;
	GetLocalTime(&t);
	printf("[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static int parse_addr(const char *s, BLUETOOTH_ADDRESS *a)
{
	ULONGLONG v = 0;
	int n = 0, d, c;
	for (; *s; s++)
	{
		if (*s == ':' || *s == '-') continue;
		c = *s | 32;
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else return 0;
		v = (v << 4) | (ULONGLONG)d;
		n++;
	}
	if (n != 12) return 0;
	a->ullLong = v;
	return 1;
}

/* Called by the Bluetooth service on its own thread for every pairing request from the target. */
static BOOL CALLBACK auth_cb(LPVOID param, PBLUETOOTH_AUTHENTICATION_CALLBACK_PARAMS p)
{
	BLUETOOTH_AUTHENTICATE_RESPONSE r;
	HRESULT hr;
	const char *what;
	(void)param;

	ZeroMemory(&r, sizeof(r));
	r.bthAddressRemote = p->deviceInfo.Address;
	r.authMethod = p->authenticationMethod;
	switch (p->authenticationMethod)
	{
	case BLUETOOTH_AUTHENTICATION_METHOD_LEGACY:
		r.pinInfo.pinLength = (UCHAR)strlen(g_pin);
		memcpy(r.pinInfo.pin, g_pin, r.pinInfo.pinLength);
		what = "legacy PIN";
		break;
	case BLUETOOTH_AUTHENTICATION_METHOD_NUMERIC_COMPARISON:
		r.numericCompInfo.NumericValue = p->Numeric_Value;
		what = "numeric comparison / just works";
		break;
	case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY_NOTIFICATION:
		r.passkeyInfo.passkey = p->Passkey;
		what = "passkey notification";
		break;
	default:
		r.negativeResponse = TRUE;
		what = "unsupported method, rejected";
		break;
	}
	hr = BluetoothSendAuthenticationResponseEx(NULL, &r);
	logmsg("  auth request (%s): response 0x%08lX", what, (unsigned long)hr);
	return TRUE;
}

static void register_auth(void)
{
	BLUETOOTH_DEVICE_INFO di;
	HRESULT hr;
	if (g_authReg) { BluetoothUnregisterAuthentication(g_authReg); g_authReg = NULL; }
	ZeroMemory(&di, sizeof(di));
	di.dwSize = sizeof(di);
	di.Address = g_target; /* only requests from this address reach the callback */
	hr = BluetoothRegisterForAuthenticationEx(&di, &g_authReg, auth_cb, NULL);
	if (hr != ERROR_SUCCESS) logmsg("BluetoothRegisterForAuthenticationEx failed: %lu", (unsigned long)hr);
}

/* Returns 1 and fills *out if the target answered the inquiry. */
static int inquire(HANDLE radio, BLUETOOTH_DEVICE_INFO *out)
{
	BLUETOOTH_DEVICE_SEARCH_PARAMS sp;
	BLUETOOTH_DEVICE_INFO di;
	HBLUETOOTH_DEVICE_FIND f;
	int found = 0;

	ZeroMemory(&sp, sizeof(sp));
	sp.dwSize = sizeof(sp);
	sp.fReturnUnknown = TRUE;
	sp.fIssueInquiry = TRUE;
	sp.cTimeoutMultiplier = g_scanMult;
	sp.hRadio = radio;
	ZeroMemory(&di, sizeof(di));
	di.dwSize = sizeof(di);

	f = BluetoothFindFirstDevice(&sp, &di);
	if (!f) return 0;
	do
	{
		if (di.Address.ullLong == g_target.ullLong) { *out = di; found = 1; break; }
		di.dwSize = sizeof(di);
	} while (BluetoothFindNextDevice(f, &di));
	BluetoothFindDeviceClose(f);
	return found;
}

static int pair(HANDLE radio, BLUETOOTH_DEVICE_INFO *di)
{
	HRESULT hr;
	DWORD err;

	logmsg("found \"%ls\", pairing...", di->szName);
	register_auth();
	hr = BluetoothAuthenticateDeviceEx(NULL, radio, di, NULL, MITMProtectionNotRequiredBonding);
	if (hr != ERROR_SUCCESS && hr != ERROR_NO_MORE_ITEMS) /* ERROR_NO_MORE_ITEMS = already authenticated */
	{
		logmsg("  pairing failed: %lu", (unsigned long)hr);
		return 0;
	}
	err = BluetoothSetServiceState(radio, di, &HidServiceUuid, BLUETOOTH_SERVICE_ENABLE);
	if (err != ERROR_SUCCESS)
	{
		logmsg("  enabling HID service failed: %lu", (unsigned long)err);
		return 0;
	}
	logmsg("  paired, HID service enabled");
	return 1;
}

static HANDLE open_radio(void)
{
	BLUETOOTH_FIND_RADIO_PARAMS rp;
	HANDLE radio = NULL;
	HBLUETOOTH_RADIO_FIND rf;
	rp.dwSize = sizeof(rp);
	rf = BluetoothFindFirstRadio(&rp, &radio);
	if (!rf) return NULL;
	BluetoothFindRadioClose(rf);
	return radio;
}

static int radio_ok(HANDLE radio)
{
	BLUETOOTH_RADIO_INFO ri;
	ri.dwSize = sizeof(ri);
	return BluetoothGetRadioInfo(radio, &ri) == ERROR_SUCCESS;
}

static void usage(void)
{
	printf("usage: p3pair <address> [-pin PIN] [-scan N]\n"
	       "  address   device address, e.g. 41:42:F6:48:76:C8\n"
	       "  -pin PIN  PIN for legacy pairing (default 0000)\n"
	       "  -scan N   inquiry length in 1.28 s units, 1..48 (default 4)\n");
}

int main(int argc, char **argv)
{
	HANDLE radio = NULL;
	BLUETOOTH_DEVICE_INFO di;
	DWORD err, lastPair = 0;
	int i, haveAddr = 0, connected = 0, paired = 0, waitingRadio = 0;

	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-pin") && i + 1 < argc)
		{
			if (strlen(argv[++i]) > BTH_MAX_PIN_SIZE) { printf("PIN too long\n"); return 1; }
			strcpy(g_pin, argv[i]);
		}
		else if (!strcmp(argv[i], "-scan") && i + 1 < argc)
		{
			int n = atoi(argv[++i]);
			if (n < 1 || n > 48) { usage(); return 1; }
			g_scanMult = (UCHAR)n;
		}
		else if (!haveAddr && parse_addr(argv[i], &g_target)) haveAddr = 1;
		else { usage(); return 1; }
	}
	if (!haveAddr) { usage(); return 1; }

	logmsg("watching %02X:%02X:%02X:%02X:%02X:%02X (Ctrl+C to quit)",
	       g_target.rgBytes[5], g_target.rgBytes[4], g_target.rgBytes[3],
	       g_target.rgBytes[2], g_target.rgBytes[1], g_target.rgBytes[0]);
	register_auth();

	for (;;)
	{
		if (!radio || !radio_ok(radio))
		{
			if (radio) { CloseHandle(radio); radio = NULL; }
			radio = open_radio();
			if (!radio)
			{
				if (!waitingRadio) logmsg("no Bluetooth radio, waiting...");
				waitingRadio = 1;
				Sleep(3000);
				continue;
			}
			waitingRadio = 0;
		}

		ZeroMemory(&di, sizeof(di));
		di.dwSize = sizeof(di);
		di.Address = g_target;
		err = BluetoothGetDeviceInfo(radio, &di);

		if (err == ERROR_SUCCESS && di.fConnected)
		{
			if (!connected) logmsg("connected");
			connected = 1;
			paired = 0;
			Sleep(1000);
			continue;
		}
		if (connected) logmsg("disconnected, scanning...");
		connected = 0;

		if (paired && GetTickCount() - lastPair < GRACE_MS) { Sleep(500); continue; }
		paired = 0;

		/* The controller forgets the link key when re-entering pairing mode, so a
		   remembered-but-disconnected entry is stale and would block re-pairing. */
		if (err == ERROR_SUCCESS && di.fRemembered)
		{
			err = BluetoothRemoveDevice(&g_target);
			logmsg("removed stale pairing (%lu)", (unsigned long)err);
		}

		if (inquire(radio, &di) && pair(radio, &di))
		{
			paired = 1;
			lastPair = GetTickCount();
		}
	}
}
