import re
import os

def process_file(file_path):
    with open(file_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Ищем блоки Cell {...}
    # Это упрощенный парсер, который ищет Cell и проверяет интерактивность
    # Лучше делать это построчно
    
    new_lines = []
    lines = content.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if 'Cell {' in line:
            # Ищем, есть ли в этом блоке clickable: true
            # Читаем блок до закрывающей скобки (упрощенно)
            block = []
            j = i
            is_clickable = False
            has_interactive_element = False
            
            while j < len(lines):
                block.append(lines[j])
                if 'clickable: true' in lines[j]:
                    is_clickable = True
                if any(x in lines[j] for x in ['GlassField', 'GlassComboBox', 'Toggle', 'NumberField', 'Segmented']):
                    has_interactive_element = True
                
                if '}' in lines[j] and j > i:
                    break
                j += 1
            
            if not is_clickable and has_interactive_element and 'interactive: true' not in '\n'.join(block):
                # Добавляем interactive: true после 'Cell {'
                new_lines.append(lines[i])
                new_lines.append('                  interactive: true')
                new_lines.extend(lines[i+1:j+1])
                i = j + 1
                continue
        
        new_lines.append(line)
        i += 1
        
    with open(file_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(new_lines))

process_file(r'C:\Users\Konstantin\Desktop\selfvoy\client\qml\BookingPage.qml')
process_file(r'C:\Users\Konstantin\Desktop\selfvoy\client\qml\PackagesPage.qml')
