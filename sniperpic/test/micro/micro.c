#include <stdio.h>
#include "sim_api.h"
int main()
{
  SimSetThreadName("main");
  SimRoiStart();
  SimRoiEnd();
  return 0;
}
