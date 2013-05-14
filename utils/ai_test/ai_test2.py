#!/usr/bin/env python
from subprocess import Popen,PIPE
from time import clock, time
from datetime import datetime
import ConfigParser
import os
import string
import random
import sys

class GameResult:
	ai_config1 = ''
	ai_config2 = ''
	ai_ident1 = ''
	ai_ident2 = ''
	duration = '0'
	faction1 = ''
	faction2 = ''
	is_success = 'false'
	local_modifications = 'false'
	map = ''
	repo_release = '0'
	test = 'default'
	end_turn = '0'
	version_string = ''
	winner_side = '0'

	def __init__(self,_ai_config1,_ai_config2,_faction1,_faction2,_map,_test):
		self.ai_config1 = _ai_config1
		self.ai_config2 = _ai_config2
		self.faction1 = _faction1
		self.faction2 = _faction2
		self.map = _map
		self.test = _test

def filter_non_printable(str):
	return ''.join(c for c in str if ord(c) > 31 or ord(c) == 9)

def construct_command_line(cfg,ai1,ai2,f1,f2,map):
	wesnoth = cfg.get('default','path_to_wesnoth_binary')
	options= cfg.get('default','additional_arguments')
	ai_config1='--ai-config 1:'+ai1
	ai_config2='--ai-config 2:'+ai2
	if sys.argv[1]=='-p':
		gui=''
	else:
		gui='--nogui'	

	if (map==''):
		optmap=''
	else:
		optmap='--scenario='+map
	
	statics='--log-info=ai/testing,mp/connect --multiplayer'
	return wesnoth+' '+options+' '+optmap+' '+ai_config1+' '+ai_config2+' '+gui+' '+statics

def do_filter(str,substring):
	n = str.find(substring)
	if (n>-1):
		return n,str[n+len(substring):].strip()
	return n,''

def run_game(cfg,game_result):

	command_line = construct_command_line(cfg,game_result.ai_config1,game_result.ai_config2, game_result.faction1, game_result.faction2, game_result.map)
	print 'Running: '+command_line
	start = time()
	p = Popen(command_line, shell=True, bufsize=10000000, stdout=PIPE, stderr=PIPE)
	# outlines = p.stdout.readlines()
	outerrlines = p.stderr.readlines()
	print 'Finished'
	for line in outerrlines:
		str = filter_non_printable(line.strip())
		n,s = do_filter(str,'info ai/testing: WINNER:')
		if (n>-1):
			print 'AND THE WINNER IS: '+s
			game_result.winner_side = s
			game_result.is_success = 'true'
			continue

		n,s = do_filter(str,'info ai/testing: VERSION:')
		if (n>-1):
			#print 'AND THE VERSION IS: '+s
			game_result.version_string = s
			n1 = s.rfind('(')
			n2 = s.rfind(')')
			if ((n1>-1) and (n2>-1) and (n2>n1)):
				sz = s[n1+1:n2]
				#parse local_modifications
				n3 = sz.rfind('M')
				if (n3>-1):
					sz = sz[:n3]
					game_result.local_modifications = 1
				#parse repo_release
				game_result.repo_release = sz
			continue

		n,s = do_filter(str,'info ai/testing: GAME_END_TURN:')
		if (n>-1):
			print 'AND THE VICTORY_TURN IS: '+s
			game_result.end_turn = s
			continue

		n,s = do_filter(str,'info ai/testing: AI_IDENTIFIER1:')
		if (n>-1):
			#print 'AND THE AI_IDENTIFIER1 IS: '+s
			game_result.ai_ident1 = s.strip()
			continue

		n,s = do_filter(str,'info ai/testing: AI_IDENTIFIER2:')
		if (n>-1):
			#print 'AND THE AI_IDENTIFIER2 IS: '+s
			game_result.ai_ident2 = s.strip()
			continue

		n,s = do_filter(str,'info mp/connect: FACTION1:')
		if (n>-1):
			#print 'AND THE FACTION1 IS: '+s
			game_result.faction1 = s
			continue

		n,s = do_filter(str,'info mp/connect: FACTION2:')
		if (n>-1):
			#print 'AND THE FACTION2 IS: '+s
			game_result.faction2 = s
			continue

	game_result.duration = time() - start
	if (game_result.is_success=='false'):
		print 'Warning: not success!'
		print '===================='
		print 'stderr:'
		for line in outerrlines:
			print filter_non_printable(line.strip())
		print '===================='

	return game_result

def save_result(cfg,log_file,game_result):
	print 'Saving to log file....'
	print 'game duration: '+str(game_result.duration);
	log_file.write('"'+game_result.ai_config1+'", "'+game_result.ai_config2+'", "'+game_result.ai_ident1+'", "'+game_result.ai_ident2+'", "'+ str(game_result.duration)+'", "'+game_result.faction1+'", "'+game_result.faction2+'", "'+str(game_result.is_success)+'", "'+str(game_result.local_modifications)+'", "'+game_result.map+'", "'+str(game_result.repo_release)+'", "'+str(game_result.test)+'", "'+str(game_result.end_turn)+'", "'+str(game_result.version_string)+'", "'+str(game_result.winner_side)+'"\n');
	log_file.flush();
	print 'Saved to log file'

def maps(cfg):
	mp = 1
	while 1:
		try:
			yield cfg.get('default','map' + repr(mp));
			mp= mp+1
		except:
			return

def tests(cfg):
	ai1=cfg.get('default','ai_config1').strip()
	ai2=cfg.get('default','ai_config2').strip()
	f1=cfg.get('default','faction1').strip()
	f2=cfg.get('default','faction2').strip()
	n=cfg.getint('default','number_of_tests')
	maplist = []
	for map in maps(cfg):
		maplist.append(map)
	random.seed()
	for i in range(0, n):
		map = random.choice(maplist)
		#d = random.randint(0,1)
		d = 0
		print 'TEST: map '+map+' i='+str(i)+' d='+str(d)
		if (d==0):
			game_result = GameResult(ai1,ai2,f1,f2,map,'default')
		else:
			game_result = GameResult(ai2,ai1,f2,f1,map,'default')
		yield game_result

# main

cfg = ConfigParser.ConfigParser()
cfg.read('ai_test2.cfg')

# only 'test the test' with GUI / start one game then exit
if sys.argv[1] == '-p':
	test = tests(cfg).next()
	run_game(cfg, test)
	sys.exit(0)
	

log_file = open(datetime.now().strftime(cfg.get('default','log_file').strip())  , 'w')
log_file.write('"ai_config1"'+', '+'"ai_config2"'+', '+'"ai_ident1"'+', '+'"ai_ident2"'+', '+ '"duration"'+', '+'"faction1"'+', '+'"faction2"'+', '+'"is_success"'+', '+'"local_modifications"'+', '+'"map"'+', '+'"repo_release"'+', '+'"test"'+', '+'"end_turn"'+', '+'"version_string"'+', '+'"winner_side"'+'\n');
log_file.flush();

total = 0
s1 = 0
s2 = 0
draw = 0

for test in tests(cfg):
	game_result = run_game(cfg,test)
	total = total + 1
	if(game_result.winner_side == '1'): s1 = s1 + 1
	if(game_result.winner_side == '2'): s2 = s2 + 1
	if(game_result.winner_side == '0'): draw = draw + 1
	print '=====Total games: ' + str(total) + ' Side 1 won: ' + str(s1) + "/" + str(s1 * 100 / total) + '% Side 2 won: ' + str(s2) + "/" + str(s2 * 100 / total) + '%' + '% Draws: ' + str(draw) + "/" + str(draw * 100 / total) + '%' 
	save_result(cfg,log_file,game_result)
