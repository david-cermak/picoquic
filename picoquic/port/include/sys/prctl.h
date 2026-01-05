#ifndef __PRCTL_H__
#define __PRCTL_H__

#define PR_SET_NAME 15
int prctl(int op, ...);

#endif