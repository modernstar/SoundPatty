# vim:ts=4:sw=4:noet:ft=python:
APPNAME='SoundPatty'
VERSION='0.2'

def set_options(opt):
	opt.tool_options('compiler_cxx')
	opt.add_option('--disable', action='store')

def configure(conf):
	print('→ configuring the project')

	conf.check_cfg(atleast_pkgconfig_version='0.0.0')
	conf.check_cfg(package='jack', args='--libs', uselib_store="JACK")

	conf.check_tool('compiler_cxx')
	conf.write_config_header('config.h')

def build(bld):
	bld.use_the_magic()

	bld(features     = ['cxx', 'cprogram'],
		target       = 'main',
		source		 = 'main.cpp logger.cpp soundpatty.cpp wavinput.cpp jackinput.cpp',
		uselib		= 'JACK',
		cxxflags      = ['-Wall', '-g'],
	)
#print ('building SoundPatty')