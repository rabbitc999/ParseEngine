' if 里套 while、while 里再套 if：跳转回填要多层嵌套也不乱
let n = 3;
if n > 0 then
    let i = 0;
    while i < n do
        if i % 2 == 0 then
            print i;
        else
            print 0 - i;
        end;
        i = i + 1;
    end;
else
    print 999;
end;
