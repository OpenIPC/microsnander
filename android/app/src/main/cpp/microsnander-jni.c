/*
 * microsnander-jni.c — the operations of src/ops.c for the Android app.
 * Java opens the USB device with the USB manager (the user allows it) and
 * hands over its file descriptor and IDs; every call runs one operation
 * and answers a Native.Result. Progress goes to a Native.Progress.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ops.h"
#include "voltage.h"
#include "usb.h"

struct cb { JNIEnv *env; jobject obj; jmethodID m; };

static void on_progress(void *ctx, const char *phase, unsigned long long done, unsigned long long total)
{
	struct cb *c = ctx;

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
	int sr = c->sr_valid ? (c->sr[0] << 16) | (c->sr[1] << 8) | c->sr[2] : -1;

	return (*env)->NewObject(env, k, init, (jboolean)ok, (*env)->NewStringUTF(env, msg),
				 (*env)->NewStringUTF(env, c->programmer), (*env)->NewStringUTF(env, c->chip),
				 (jlong)c->size, (jint)sr, (*env)->NewStringUTF(env, name ? name : ""),
				 (*env)->NewStringUTF(env, c->chip[0] ? ms_voltage_name(c->voltage) : ""),
				 (*env)->NewStringUTF(env, c->chip[0] ? c->warning : ""));
}

static void use_device(jint fd, jint vid, jint pid)
{
	ms_usb_fd = fd;
	ms_usb_vid = (uint16_t)vid;
	ms_usb_pid = (uint16_t)pid;
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

	use_device(fd, vid, pid);
	int ret = ms_detect(&c, msg, sizeof(msg));
	return result(env, ret == 0, msg, &c, NULL);
}

/* the whole chip into path; the result names the dump as the web mode does */
JNIEXPORT jobject JNICALL FN(read)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid, jstring jpath, jobject progress)
{
	struct ms_chip c;
	char msg[256] = "", name[160] = "";
	struct cb cb = make_cb(env, progress);
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	FILE *f = fopen(path, "wb");
	int ret = -1;
	(void)cls;

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
	return result(env, ret == 0, msg, &c, name);
}

JNIEXPORT jobject JNICALL FN(erase)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid, jobject progress)
{
	struct ms_chip c;
	char msg[256] = "";
	struct cb cb = make_cb(env, progress);
	(void)cls;

	use_device(fd, vid, pid);
	int ret = ms_erase(&c, on_progress, &cb, msg, sizeof(msg));
	return result(env, ret == 0, msg, &c, NULL);
}

/* the file at path from address 0, then verified */
JNIEXPORT jobject JNICALL FN(write)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid, jstring jpath, jobject progress)
{
	struct ms_chip c;
	char msg[256] = "";
	struct cb cb = make_cb(env, progress);
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	FILE *f = fopen(path, "rb");
	unsigned char *data = NULL;
	long len = -1;
	int ret = -1;
	(void)cls;

	use_device(fd, vid, pid);
	memset(&c, 0, sizeof(c));
	if (f && !fseek(f, 0, SEEK_END) && (len = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) &&
	    (data = malloc(len)) && fread(data, 1, len, f) == (size_t)len)
		ret = ms_write(&c, data, len, on_progress, &cb, msg, sizeof(msg));
	else
		snprintf(msg, sizeof(msg), len == 0 ? "The file is empty." : "Cannot read the file.");
	free(data);
	if (f)
		fclose(f);
	(*env)->ReleaseStringUTFChars(env, jpath, path);
	return result(env, ret == 0, msg, &c, NULL);
}

JNIEXPORT jobject JNICALL FN(registers)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid)
{
	struct ms_chip c;
	char msg[256] = "";
	(void)cls;

	use_device(fd, vid, pid);
	int ret = ms_registers(&c, msg, sizeof(msg));
	return result(env, ret == 0, msg, &c, NULL);
}

JNIEXPORT jobject JNICALL FN(unprotect)(JNIEnv *env, jclass cls, jint fd, jint vid, jint pid)
{
	struct ms_chip c;
	char msg[256] = "";
	(void)cls;

	use_device(fd, vid, pid);
	int ret = ms_unprotect(&c, msg, sizeof(msg));
	return result(env, ret == 0, msg, &c, NULL);
}
