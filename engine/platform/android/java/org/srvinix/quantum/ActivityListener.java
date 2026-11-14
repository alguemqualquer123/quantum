package org.srvinix.quantum;

// Activity lifecycle for the optional services MainActivity creates
interface ActivityListener {
	default void onActivityResume() {}
	default void onActivityPause() {}
	default void onActivityDestroy() {}
}
