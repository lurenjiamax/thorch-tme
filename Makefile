obj-m += aynthor_qmp.o tme_overlay_loader.o aynthor_tme.o
aynthor_tme-y := tme_client.o tme_broadcast.o
ccflags-y += -I$(src)/include
