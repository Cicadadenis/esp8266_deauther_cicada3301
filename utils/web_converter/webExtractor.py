#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Web files extractor - распаковывает сжатые файлы из web в web2

import os
import gzip
from pathlib import Path
from shutil import rmtree

def extract_web_files(web_path, output_path="web2"):
    """
    Распаковывает все .gz файлы из папки web в папку web2
    с сохранением структуры папок
    """
    
    web_dir = Path(web_path)
    output_dir = Path(output_path)
    
    # Создаем папку web2
    if output_dir.exists():
        print(f"[!] Папка '{output_path}' уже существует, удаляю...")
        rmtree(output_dir)
    
    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"[+] Создана папка '{output_path}'\n")
    
    # Список для отслеживания распакованных файлов
    extracted_count = 0
    
    # Рекурсивно ищем все .gz файлы
    for gz_file in web_dir.rglob("*.gz"):
        # Определяем относительный путь
        rel_path = gz_file.relative_to(web_dir)
        
        # Путь для распакованного файла (без .gz)
        output_file = output_dir / rel_path.with_suffix('')
        
        # Создаем необходимые директории
        output_file.parent.mkdir(parents=True, exist_ok=True)
        
        try:
            # Распаковываем файл
            with gzip.open(gz_file, 'rb') as f_in:
                content = f_in.read()
            
            # Сохраняем распакованный файл
            with open(output_file, 'wb') as f_out:
                f_out.write(content)
            
            print(f"[+] Распакован: {rel_path} → {output_file.name}")
            extracted_count += 1
            
        except Exception as e:
            print(f"[!] Ошибка при распаковке {rel_path}: {e}")
    
    print(f"\n[+] Всего распаковано файлов: {extracted_count}")
    print(f"[+] Файлы сохранены в папку: {output_dir.absolute()}")

if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description="Распаковывает сжатые веб-файлы")
    parser.add_argument("--webpath", type=str, default="web",
                        help='Путь к папке web (по умолчанию: web)')
    parser.add_argument("--output", type=str, default="web2",
                        help='Путь для выходной папки (по умолчанию: web2)')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.webpath):
        print(f"[!] Папка '{args.webpath}' не найдена!")
        exit(1)
    
    print("\n=== Web Files Extractor ===\n")
    extract_web_files(args.webpath, args.output)
    print("\n[+] Готово!")
