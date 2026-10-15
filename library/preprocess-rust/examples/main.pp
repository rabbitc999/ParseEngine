%symbol APP
%include "common.inc"

%if APP
app on
%else
app off
%endif

%if DEBUG
%include "debug.inc"
%else
/* 没有任何调试输出 */
%endif

%symbol TAIL 1
%if TAIL
done
%endif
