// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef AndroidServices_H_
#define AndroidServices_H_

#include <jni.h>

// Finds the AdMob and Google Play Billing wrappers the export compiled in and registers their natives
void setupServicesJNI(JNIEnv* env, jobject activity, jclass activityClass);
void releaseServicesJNI(JNIEnv* env);

#endif /* AndroidServices_H_ */
