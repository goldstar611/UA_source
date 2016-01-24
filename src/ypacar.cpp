#include <inttypes.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "classes.h"
#include "ypacar.h"


stored_functions *classvtbl_get_ypacar();
class_return * class_set_ypacar(int, ...);

stored_functions ypacar_class_vtbl(class_set_ypacar);


class_stored ypacar_class_off (NULL, NULL, "MC2classes:ypacar.class", classvtbl_get_ypacar);


stored_functions *classvtbl_get_ypacar()
{
    return &ypacar_class_vtbl;
}

CLASSFUNC ypacar_funcs[1024];




NC_STACK_ypacar * ypacar_func0(class_stru *clss, class_stru *zis, stack_vals *stak)
{
    NC_STACK_ypacar *obj = (NC_STACK_ypacar *)call_parent(zis, clss, 0, stak);

    return obj;
}

size_t ypacar_func1(NC_STACK_ypacar *obj, class_stru *zis, stack_vals *stak)
{
    return call_parent(zis, obj, 1, stak);
}

void ypacar_func2(NC_STACK_ypacar *obj, class_stru *zis, stack_vals *stak)
{
    call_parent(zis, obj, 2, stak);
}

void ypacar_func3(NC_STACK_ypacar *obj, class_stru *zis, stack_vals *stak)
{
    call_parent(zis, obj, 3, stak);
}




class_return ypacar_class_descr;

class_return * class_set_ypacar(int , ...)
{

    memset(ypacar_funcs, 0, sizeof(CLASSFUNC) * 1024);

    ypacar_class_descr.parent = "ypatank.class";

    ypacar_funcs[0] = (CLASSFUNC)ypacar_func0;
    ypacar_funcs[1] = (CLASSFUNC)ypacar_func1;
    ypacar_funcs[2] = (CLASSFUNC)ypacar_func2;
    ypacar_funcs[3] = (CLASSFUNC)ypacar_func3;

    ypacar_class_descr.vtbl = ypacar_funcs;
    ////ypacar_class_descr.varSize = sizeof(__NC_STACK_ypacar);
    ypacar_class_descr.varSize = sizeof(NC_STACK_ypacar) - offsetof(NC_STACK_ypacar, stack__ypacar); //// HACK
    ypacar_class_descr.field_A = 0;
    return &ypacar_class_descr;
}
