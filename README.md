# Virtual serial driver

This sample driver is a minimal driver meant to demonstrate the usage of the User-Mode Driver Framework. 

## Code tour

### internal.h

- This is the main header file for the sample driver.

### driver.cpp and driver.h

- Definition and implementation of the driver callback class (CMyDriver) for the sample. This includes **DriverEntry** and events on the framework driver object.

### device.cpp and driver.h

- Definition and implementation of the device callback class (CMyDriver) for the sample. This includes events on the framework device object.

### queue.cpp and queue.h

- Definition and implementation of the base queue callback class (CMyQueue). This includes events on the framework I/O queue object.

### VirtualSerial.inf / FakeModem.inf

- INF file that contains installation information for this driver.
