#
# Regular cron jobs for the handle-core package
#
0 4	* * *	root	[ -x /usr/bin/handle-core_maintenance ] && /usr/bin/handle-core_maintenance
