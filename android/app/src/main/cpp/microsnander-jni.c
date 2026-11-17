/*
 * microsnander-jni.c — the operations of src/ops.c for the Android app.
 * Java opens the USB device with the USB manager (the user allows it) and
 * hands over its file descriptor and IDs; every call runs one operation
 * and answers a Native.Result. Progress goes to a Native.Progress.
 *
 * The web mode (src/web.c) runs here too, for remote access: webServe
 * blocks in a thread of the service until webStop; each operation of the
 * page asks the service for the USB device (RemoteService.openDevice) and
 * gives it back after. The app's own operations take the same slot as the
 * page's, so only one runs at a time, and the page shows them too.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ops.h"
#include "voltage.h"
#include "usb.h"
#include "web.h"

const char *ms_version = MS_VERSION;	/* web.c shows it on the page */

static JavaVM *vm;
static jclass service;			/* RemoteService, for the device of the page's operations */
static jmethodID open_device, close_device;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *v, void *reserved)
{
	JNIEnv *env;
	(void)reserved;

	vm = v;
	if ((*v)->GetEnv(v, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
		return JNI_ERR;
	jclass k = (*env)->FindClass(env, "org/openipc/microsnander/RemoteService");
	if (!k)
		return JNI_ERR;
	service = (*env)->NewGlobalRef(env, k);
	open_device = (*env)->GetStaticMethodID(env, k, "openDevice", "()J");
	close_device = (*env)->GetStaticMethodID(env, k, "closeDevice", "()V");
	if (!open_device || !close_device)
		return JNI_ERR;
	return JNI_VERSION_1_6;
}

/* the env of this thread, attached to the VM when it is one of web.c's */
static JNIEnv *thread_env(int *attached)
{
	JNIEnv *env = NULL;

	*attached = 0;
	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
		if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK)
			return NULL;
		*attached = 1;
	}
	return env;
}

/* the page's operation: the service opens the programmer, fd << 32 | vid << 16 | pid, or -1 */
static int page_device_open(void)
{
	int attached;
	JNIEnv *env = thread_env(&attached);
	jlong d = -1;

	if (env) {
		d = (*env)->CallStaticLongMethod(env, service, open_device);
		if ((*env)->ExceptionCheck(env)) {
			(*env)->ExceptionClear(env);
			d = -1;
		}
		if (attached)
			(*vm)->DetachCurrentThread(vm);
	}
	if (d < 0)
		return -1;
	ms_usb_fd = (int)(d >> 32);
	ms_usb_vid = (uint16_t)(d >> 16);
	ms_usb_pid = (uint16_t)d;
	return 0;
}

static void page_device_close(void)
{
	int attached;
	JNIEnv *env = thread_env(&attached);

	ms_usb_fd = -1;
	if (!env)
		return;
	(*env)->CallStaticVoidMethod(env, service, close_device);
	if ((*env)->ExceptionCheck(env))
		(*env)->ExceptionClear(env);
	if (attached)
		(*vm)->DetachCurrentThread(vm);
}

struct cb { JNIEnv *env; jobject obj; jmethodID m; };

static void on_progress(void *ctx, const char *phase, unsigned long long done, unsigned long long total)
{
	struct cb *c = ctx;

	web_local_progress(phase, done, total);
	if (!c->obj)
		return;
	jstring p = (*c->env)->NewStringUTF(c->env, phase);
	(*c->env)->CallVoidMethod(c->env, c->obj, c->m, p, (jlong)done, (jlong)total);
	(*c->env)->DeleteLocalRef(c->env, p);
}

/* new Native.Result(ok, message, programmer, chip, size, sr or -1, name, voltage, warning) */
static jobject result(JNIEnv *env, int ok, const char *msg, const struct ms_chip *c, const char *name)
{
	jclass k = (*env)->FindClass(env, "org/openipc/microsnander/Native$Result");
	jmethodID init = (*env)->GetMethodID(env, k, "<init>",
		"(ZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;JILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
	int sr = c->sr_valid ? (c->sr_macronix << 24) | (c->sr[0] << 16) | (c->sr[1] << 8) | c->sr[2] : -1;

	return (*env)->NewObject(env, k, init, (jboolean)ok, (*env)->NewStringUTF(env, msg),
				 (*env)->NewStringUTF(env, c->programmer), (*env)->NewStringUTF(env, c->chip),
				 (jlong)c->size, (jint)sr, (*env)->NewStringUTF(env, name ? name : ""),
				 (*env)->NewStringUTF(env, c->chip[0] ? ms_voltage_name(c->voltage) : ""),
				 (*env)->NewStringUTF(env, c->chip[0] ? c->warning : ""));
}

/* a remote operation holds the programmer: this one does not run */
static jobject busy_result(JNIEnv *env)
{
	struct ms_chip c;

	memset(&c, 0, sizeof(c));
	return result(env, 0, "A remote operation is running: wait for it to end.", &c, NULL);
}

static void use_device(jint fd, jint vid, jint pid)
{
	ms_usb_fd = fd;
	ms_usb_vid = (uint16_t)vid;
	ms_usb_pid = (uint16_t)pid;
}

/* The chip the user confirmed (Native.expect) waits here until its operation
   holds the shared slot: a page's operation between the two cannot take it
   or change it. */
static pthread_mutex_t expect_lock = PTHREAD_MUTEX_INITIALIZER;
static char expect_chip[64];
static long long expect_size;
static int expect_set;

/* the pending expectation: to ms_expect for a confirmed operation (apply), or dropped */
static void take_expect(int apply)
{
	pthread_mutex_lock(&expect_lock);
	if (apply && expect_set)
		ms_expect(expect_chip, expect_size);
	expect_set = 0;
	pthread_mutex_unlock(&expect_lock);
}

/* the shared slot for op: 0, or -1 when the page runs one (the confirmation is dropped) */
static int local_begin(int op, int confirmed)
{
	if (web_local_begin(op)) {
		take_expect(0);
		return -1;
	}
	take_expect(confirmed);
	return 0;
}

static struct cb make_cb(JNIEnv *env, jobject progress)
{
	struct cb c = { env, progress, NULL };

	if (progress) {
		jclass k = (*env)->GetObjectClass(env, progress);
		c.m = (*env)->GetMethodID(env, k, "progress", "(Ljava/lang/String;JJ)V");
	}
	return c;
}

#define FN(name) Java_org_openipc_microsnander_Native_##name

JNIEXPORT jobject JNICALL FN(detect)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid)
{
	struct ms_chip c;
	char msg[256] = "";
	(void)cls;

	if (local_begin(OP_DETECT, 0))
		return busy_result(env);

	use_device(fd, vid, pid);
	int ret = ms_detect(&c, msg, sizeof(msg));
	web_local_end(&c, ret == 0, msg, NULL);
	return result(env, ret == 0, msg, &c, NULL);
}

/* the whole chip into path; the result names the dump as the web mode does */
JNIEXPORT jobject JNICALL FN(read)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid, jstring jpath, jobject progress)
{
	struct ms_chip c;
	char msg[256] = "", name[160] = "";
	struct cb cb = make_cb(env, progress);
	int ret = -1;
	(void)cls;

	if (local_begin(OP_READ, 0))		/* before the file: a busy read keeps the last dump */
		return busy_result(env);
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	FILE *f = fopen(path, "wb");

	use_device(fd, vid, pid);
	memset(&c, 0, sizeof(c));
	if (!f)
		snprintf(msg, sizeof(msg), "Cannot create the dump file.");
	else {
		ret = ms_read(&c, f, 0, 0, on_progress, &cb, msg, sizeof(msg));
		if (fclose(f) && !ret) {
			snprintf(msg, sizeof(msg), "Cannot write the dump file.");
			ret = -1;
		}
		if (!ret)
			ms_dump_name(name, sizeof(name), c.chip, 0, 0, 0);
	}
	(*env)->ReleaseStringUTFChars(env, jpath, path);
	web_local_end(&c, ret == 0, msg, NULL);
	return result(env, ret == 0, msg, &c, name);
}

JNIEXPORT jobject JNICALL FN(erase)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid, jobject progress)
{
	struct ms_chip c;
	char msg[256] = "";
	struct cb cb = make_cb(env, progress);
	(void)cls;

	if (local_begin(OP_ERASE, 1))
		return busy_result(env);

	use_device(fd, vid, pid);
	int ret = ms_erase(&c, on_progress, &cb, msg, sizeof(msg));
	web_local_end(&c, ret == 0, msg, NULL);
	return result(env, ret == 0, msg, &c, NULL);
}

/* -t on the first block of the chip expected; ok only for the OK verdict */
JNIEXPORT jobject JNICALL FN(selftest)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid, jstring jdir)
{
	struct ms_chip c;
	char msg[512] = "", saved[160] = "";
	int ret;
	(void)cls;

	if (local_begin(OP_SELFTEST, 1))
		return busy_result(env);
	const char *dir = (*env)->GetStringUTFChars(env, jdir, NULL);

	use_device(fd, vid, pid);
	mkdir(dir, 0700);
	ret = ms_selftest(&c, dir, saved, sizeof(saved), msg, sizeof(msg));
	(*env)->ReleaseStringUTFChars(env, jdir, dir);
	web_local_end(&c, ret == 0, msg, saved);
	return result(env, ret == 0, msg, &c, saved);	/* name: the block saved in dir, "" */
}

/* the chip the next operation must find (the one the user confirmed): it binds
   that operation once it holds the shared slot */
JNIEXPORT void JNICALL FN(expect)(JNIEnv *env, jclass cls, jstring jchip, jlong size)
{
	const char *chip = (*env)->GetStringUTFChars(env, jchip, NULL);
	(void)cls;

	pthread_mutex_lock(&expect_lock);
	snprintf(expect_chip, sizeof(expect_chip), "%s", chip ? chip : "");
	expect_size = (long long)size;
	expect_set = 1;
	pthread_mutex_unlock(&expect_lock);
	(*env)->ReleaseStringUTFChars(env, jchip, chip);
}

/* the file at path from address 0, then verified; read in parts, not kept in memory */
JNIEXPORT jobject JNICALL FN(write)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid, jstring jpath, jobject progress)
{
	struct ms_chip c;
	char msg[256] = "";
	struct cb cb = make_cb(env, progress);
	int ret = -1;
	(void)cls;

	if (local_begin(OP_WRITE, 1))
		return busy_result(env);
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	FILE *f = fopen(path, "rb");

	use_device(fd, vid, pid);
	memset(&c, 0, sizeof(c));
	if (f)
		ret = ms_write_file(&c, f, on_progress, &cb, msg, sizeof(msg));
	else {
		ms_expect(NULL, 0);		/* no write: the next operation is not bound to the chip */
		snprintf(msg, sizeof(msg), "Cannot read the file.");
	}
	if (f)
		fclose(f);
	(*env)->ReleaseStringUTFChars(env, jpath, path);
	web_local_end(&c, ret == 0, msg, NULL);
	return result(env, ret == 0, msg, &c, NULL);
}

JNIEXPORT jobject JNICALL FN(registers)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid)
{
	struct ms_chip c;
	char msg[256] = "";
	(void)cls;

	if (local_begin(OP_REGISTERS, 0))
		return busy_result(env);

	use_device(fd, vid, pid);
	int ret = ms_registers(&c, msg, sizeof(msg));
	web_local_end(&c, ret == 0, msg, NULL);
	return result(env, ret == 0, msg, &c, NULL);
}

JNIEXPORT jobject JNICALL FN(unprotect)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid)
{
	struct ms_chip c;
	char msg[256] = "";
	(void)cls;

	if (local_begin(OP_UNPROTECT, 1))
		return busy_result(env);

	use_device(fd, vid, pid);
	int ret = ms_unprotect(&c, msg, sizeof(msg));
	web_local_end(&c, ret == 0, msg, NULL);
	return result(env, ret == 0, msg, &c, NULL);
}

JNIEXPORT jobject JNICALL FN(writeRegister)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid, jint reg, jint mask, jint value)
{
	struct ms_chip c;
	char msg[256] = "";
	(void)cls;

	if (local_begin(OP_REGISTER, 1))
		return busy_result(env);

	use_device(fd, vid, pid);
	int ret = ms_write_register(&c, reg, (unsigned int)mask, (unsigned int)value, msg, sizeof(msg));
	web_local_end(&c, ret == 0, msg, NULL);
	return result(env, ret == 0, msg, &c, NULL);
}

/* ---- the web mode, for remote access ---- */

/* serves the page on listen ("[address:]port") until webStop; 0, or 1 when it cannot listen */
JNIEXPORT jint JNICALL FN(webServe)(JNIEnv *env, jclass cls, jstring jlisten, jstring jpassword, jstring jdir)
{
	const char *listen = (*env)->GetStringUTFChars(env, jlisten, NULL);
	const char *password = (*env)->GetStringUTFChars(env, jpassword, NULL);
	const char *dir = (*env)->GetStringUTFChars(env, jdir, NULL);
	/* web.c keeps the password (empty: none) and the directory while it
	   serves; web_serve returns once nothing of it uses them */
	char *pw = strdup(password), *d = strdup(dir), *l = strdup(listen);
	int ret = 1;
	(void)cls;

	(*env)->ReleaseStringUTFChars(env, jlisten, listen);
	(*env)->ReleaseStringUTFChars(env, jpassword, password);
	(*env)->ReleaseStringUTFChars(env, jdir, dir);
	if (pw && d && l) {		/* an empty password: the page is open */
		mkdir(d, 0700);
		web_device_open = page_device_open;
		web_device_close = page_device_close;
		/* a tunnel (camex, nginx) comes by a name: any name with a password;
		   without one only names under the camex server (webTunnelHost), so a
		   site cannot reach the page by DNS rebinding */
		web_allow_any_host(*pw != 0);
		ret = web_serve(l, pw, d, 0, 0);
	}
	free(pw);
	free(d);
	free(l);
	return ret;
}

JNIEXPORT void JNICALL FN(webStop)(JNIEnv *env, jclass cls)
{
	(void)env;
	(void)cls;
	web_stop();
}

/* the state of the last operation, the app's or the page's: the JSON of /api/state */
JNIEXPORT jstring JNICALL FN(state)(JNIEnv *env, jclass cls)
{
	char buf[3072];
	(void)cls;

	web_state_json(buf, sizeof(buf));
	return (*env)->NewStringUTF(env, buf);
}

/* the state of the camex tunnel, for the page ("" none) */
JNIEXPORT void JNICALL FN(setTunnel)(JNIEnv *env, jclass cls, jstring jtext)
{
	const char *text = (*env)->GetStringUTFChars(env, jtext, NULL);
	(void)cls;

	web_set_tunnel(text);
	(*env)->ReleaseStringUTFChars(env, jtext, text);
}

/* a dump the app kept in the dump directory: the page shows it as the last one */
JNIEXPORT void JNICALL FN(dumpKept)(JNIEnv *env, jclass cls, jstring jname)
{
	const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
	(void)cls;

	web_show_dump(name);
	(*env)->ReleaseStringUTFChars(env, jname, name);
}

/* the camex server's host name: the page takes Host names under it without a password ("" none) */
JNIEXPORT void JNICALL FN(webTunnelHost)(JNIEnv *env, jclass cls, jstring jhost)
{
	const char *host = (*env)->GetStringUTFChars(env, jhost, NULL);
	(void)cls;

	web_allow_host_domain(host);
	(*env)->ReleaseStringUTFChars(env, jhost, host);
}
