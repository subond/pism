/*
   Copyright (C) 2012 Ed Bueler

   This file is part of PISM.

   PISM is free software; you can redistribute it and/or modify it under the
   terms of the GNU General Public License as published by the Free Software
   Foundation; either version 2 of the License, or (at your option) any later
   version.

   PISM is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
   details.

   You should have received a copy of the GNU General Public License
   along with PISM; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

/*  STANDARD DIALOGUE:

$ ./simpleP
Enter  r  (in km; 0 <= r <= L = 22.5;  e.g. 20.0):   20.0
Results from Test P:
    h = 180.0000 (m)  Po = 16.06878 (bar)  |vb| = 23.73047 (m a-1)
    W_c = 0.58184968 (m)  W = 0.67507258 (m)

*/

#include <stdio.h>
#include "exactTestP.h"

int main() {

  double       r, h, magvb, Wcrit, W;
  int          scanret, ierr;
  const double secpera=31556926.0;  /* seconds per year; 365.2422 days */

  double EPS_ABS[] = { 1.0e-12, 1.0e-9,  1.0e-7  };
  double EPS_REL[] = { 1.0e-15, 1.0e-14, 1.0e-11 };

  printf("Enter  r  (in km; 0 <= r <= L = %.1f;  e.g. 20.0):   ",L/1000.0);
  scanret = scanf("%lf",&r);
  if (scanret != 1) {
    printf("... input error; exiting\n");
    return 1;
  }

  ierr = exactP(r*1000.0,&h,&magvb,&Wcrit,&W,EPS_ABS[0],EPS_REL[0],1);
  if (ierr) {
    printf("\n\nsimpleP ENDING because of ERROR from exactP():\n");
    error_message_testP(ierr);
    return 1;
  }

  printf("Results from Test P:\n");
  printf("    h = %.4f (m)  Po = %.5f (bar)  |vb| = %.5f (m a-1)\n"
         "    W_c = %.8f (m)  W = %.8f (m)\n",
         h,910.0*9.81*h/1.0e5,magvb*secpera,Wcrit,W);

#define COMMENTARY 0
#if COMMENTARY
  char*  methodnames[4] = { "rk8pd", "rk2", "rkf45", "rkck" };
  printf("\nAbove were produced with RK Dormand-Prince (8,9) method\n"
         "and default (tight) tolerances EPS_ABS = %.1e, EPS_REL = %.1e.\n",
         EPS_ABS[0],EPS_REL[0]);
  printf("Here is a table of values using alternative methods and tolerances.\n\n");
  int method,i,j;
  for (method=1; method<5; method++) {
    printf("method = %d = %s:\n",method,methodnames[method-1]);
    for (i=0; i<3; i++) {
      printf("    EPS_ABS = %.1e\n",EPS_ABS[i]);
      for (j=0; j<3; j++) {
        ierr = exactP(r*1000.0,&h,&magvb,&Wcrit,&W,EPS_ABS[i],EPS_REL[j],method);
        if (ierr) {
          printf("\n\nsimpleP ENDING because of ERROR from exactP():\n");
          error_message_testP(ierr);
          return 1;
        }
        printf("        EPS_REL = %.1e:   W = %.14f\n",EPS_REL[j],W);
      }
    }
  }
#endif

  return 0;
}

