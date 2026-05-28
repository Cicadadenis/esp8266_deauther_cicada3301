#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys
import gzip
import logging as log
from argparse import ArgumentParser
from datetime import datetime
from functools import partial
from hashlib import sha1
from multiprocessing import Pool, cpu_count
from subprocess import getoutput
from time import sleep

from .css_minifier import css_minify
from .html_minifier import html_minify
from .js_minifier import js_minify

__version__ = '2.5.0'
__license__ = 'GPLv3+ LGPLv3+'

start_time = datetime.now()

def check_folder(folder):
    if folder and not os.path.isdir(folder):
        os.makedirs(folder)

def process_single_css_file(css_file_path, wrap=False, timestamp=False,
                            comments=False, sort=False, overwrite=False,
                            zipy=False, prefix='', add_hash=False,
                            output_path=None):
    with open(css_file_path, encoding="utf-8") as f:
        original_css = f.read()
    minified_css = css_minify(original_css, wrap=wrap, comments=comments, sort=sort)
    out = output_path or css_file_path.replace('.css', '.min.css')
    with open(out, "w", encoding="utf-8") as f:
        f.write(minified_css)
    return out

def process_single_html_file(html_file_path, comments=False, overwrite=False,
                             prefix='', add_hash=False, output_path=None):
    with open(html_file_path, encoding="utf-8") as f:
        minified_html = html_minify(f.read(), comments=comments)
    out = output_path or html_file_path.replace('.html', '.min.html')
    with open(out, "w", encoding="utf-8") as f:
        f.write(minified_html)
    return out

def process_single_js_file(js_file_path, timestamp=False, overwrite=False,
                           zipy=False, output_path=None):
    with open(js_file_path, encoding="utf-8") as f:
        original_js = f.read()
    minified_js = js_minify(original_js)
    out = output_path or js_file_path.replace('.js', '.min.js')
    with open(out, "w", encoding="utf-8") as f:
        f.write(minified_js)
    return out
