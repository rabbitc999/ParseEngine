"""验收 ParseEngine 双语、Windows 控制台编码、Unicode 路径和独立 ZIP 发布脚本。"""
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import argparse
import hashlib
import importlib.util
import os
import subprocess
import sys
import tempfile
import zipfile

ROOT=Path(__file__).resolve().parents[1]

def main():
    """通过真实控制台屏幕与重定向字节检验两种输出通道。"""
    parser=argparse.ArgumentParser(description=__doc__)
    suffix='.exe' if os.name=='nt' else ''
    parser.add_argument('--engine',type=Path,default=ROOT/('build/bin/ParseEngine'+suffix))
    parser.add_argument('--magel',type=Path,default=ROOT/('magel/magel'+suffix))
    options=parser.parse_args();engine=options.engine.resolve();magel=options.magel.resolve()
    env=os.environ.copy()
    for key in ('MAGEL_LANG','PARSEENGINE_LANG','LC_ALL','LC_MESSAGES','LANG'):env.pop(key,None)
    env['PARSEENGINE_PYTHON']=sys.executable;env['MAGEL_PYTHON']=sys.executable
    def run(*args,cwd=ROOT,extra=None):
        return subprocess.run([str(x) for x in args],cwd=cwd,env={**env,**(extra or {})},capture_output=True,encoding='utf-8',errors='strict',timeout=180)
    with tempfile.TemporaryDirectory(prefix='parseengine i18n-') as temporary:
        work=Path(temporary)
        helper=work/('console.exe' if os.name=='nt' else 'console')
        result=run('g++','-std=c++17','-finput-charset=UTF-8','-fexec-charset=UTF-8',ROOT/'tests/windows_console_test.cpp','-o',helper)
        assert result.returncode==0,result
        result=run(helper,'--bytes')
        assert result.returncode==0 and result.stdout=='C++ 中文 / 日本語\n' and result.stderr=='C stdio 中文 / 日本語\n',result
        if os.name=='nt':
            # Magel 已拆分为独立仓库：这里只顺带检查本地那份二进制，不在就跳过这一项。
            checks=[(engine,['--help','--ui-lang','en'],'Usage')]
            if magel.exists():
                checks.append((magel,['help','lang','zh'],'用法'))
            else:
                print('（跳过 Magel 检查：magel/magel 不在本地；Magel 已是独立仓库）')
            for cp in (936,932):
                result=run(helper,cp,helper,'--bytes')
                assert result.returncode==0 and 'C++ 中文 / 日本語' in result.stdout and 'C stdio 中文 / 日本語' in result.stdout,result
                for tool,arguments,word in checks:
                    result=run(helper,cp,tool,*arguments)
                    assert result.returncode==0 and word in result.stdout,result
        for code,word in (('zh','用法'),('en','Usage')):
            result=run(engine,'--help','--ui-lang',code)
            assert result.returncode==0 and word in result.stdout and '--lang' in result.stdout and '--ui-lang' in result.stdout,result
            result=run(engine,'--help',extra={'PARSEENGINE_LANG':code})
            assert result.returncode==0 and word in result.stdout,result
            result=run(engine,'--bad-中文',f'--ui-lang={code}')
            assert result.returncode==2 and '--bad-中文' in result.stderr,result
            assert {'zh':'错误','en':'Error'}[code] in result.stderr,result
        assert 'Usage' in run(engine,'--help',extra={'PARSEENGINE_LANG':'auto','LC_ALL':'ja_JP.UTF-8'}).stdout
        assert run(engine,'--help','--ui-lang','ja').returncode==2
        assert 'Usage' in run(engine,'--help',extra={'LC_ALL':'C','LANG':'ja_JP.UTF-8'}).stdout
        assert 'Usage' in run(engine,'--help','--ui-lang','en',extra={'PARSEENGINE_LANG':'ja'}).stdout
        assert run(engine,'--help','--ui-lang','invalid').returncode==2
        assert run(engine,'--ui-lang').returncode==2
        source=work/'中文-日本語.gram'
        source.write_text('%mode ielr\n%lang cpp17\n%tree\n%token HELLO "hello"\n%space " \\t\\r\\n"\n%start document\ndocument := HELLO\n',encoding='utf-8')
        snapshots=[]
        for code in ('zh','en'):
            output=work/('工程-日本語-'+code)
            result=run(engine,source,'--out-dir',output,'--lang','cpp17','--ui-lang',code,'--report',work/(code+'-报告.md'))
            assert result.returncode==0,result
            assert {'zh':'解析完成','en':'Generation complete'}[code] in result.stdout,result
            snapshots.append({p.name:p.read_bytes() for p in output.iterdir() if p.suffix in ('.cpp','.hpp','.h','.c')})
        assert snapshots[0] and snapshots[0]==snapshots[1]
        result=run(engine,work/'不存在-日本語.gram','--ui-lang','en')
        assert result.returncode==1 and 'Cannot open grammar file' in result.stderr and '不存在-日本語.gram' in result.stderr,result
        archive=work/'安装包-日本語.zip'
        result=run(engine,'zip','out',archive,'--ui-lang','en',cwd=work)
        assert result.returncode==0 and 'Packaged' in result.stdout,result
        original=engine.read_bytes();before=archive.read_bytes()
        with zipfile.ZipFile(archive) as bundle:
            name='ParseEngine.exe' if os.name=='nt' else 'ParseEngine'
            assert bundle.namelist()==[name] and bundle.testzip() is None
            assert hashlib.sha256(bundle.read(name)).digest()==hashlib.sha256(original).digest()
            assert bundle.getinfo(name).compress_type==zipfile.ZIP_DEFLATED
            extracted=work/'outside';bundle.extractall(extracted)
        if os.name!='nt':(extracted/name).chmod(0o755)
        result=run(extracted/name,'--help','--ui-lang','en',cwd=extracted)
        assert result.returncode==0 and 'Usage' in result.stdout,result
        result=run(extracted/name,'zip','out',work/'再次打包.zip','--ui-lang','en',cwd=extracted)
        assert result.returncode==0 and 'Packaged' in result.stdout,result
        assert run(engine,'zip','out',archive).returncode!=0 and archive.read_bytes()==before
        assert run(engine,'zip','out',work/'wrong.txt').returncode!=0 and not (work/'wrong.txt').exists()
        assert run(engine,'zip','out',work/'missing'/'out.zip').returncode!=0
        assert run(engine,'zip','out').returncode==2
        result=run(sys.executable,ROOT/'tools/package_compiler.py','ParseEngine','out',work/'脚本.zip','--compiler',engine,'--ui-lang','zh')
        assert result.returncode==0 and '已打包' in result.stdout,result
        spec=importlib.util.spec_from_file_location('package_compiler',ROOT/'tools/package_compiler.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        partial=work/'partial.zip'
        with patch.object(zipfile.ZipFile,'write',side_effect=OSError('injected write failure')):
            try:module.package(engine,partial,'ParseEngine')
            except OSError:pass
            else:raise AssertionError('写入失败未上报')
        assert not partial.exists()
        console='Windows 控制台代码页 936、932' if os.name=='nt' else '重定向 UTF-8（本机不运行 Windows 控制台检查）'
        print(f'ParseEngine 双语 / Unicode / ZIP 通过；{console}；ZIP {len(original)} -> {len(before)} 字节')

if __name__=='__main__':main()
