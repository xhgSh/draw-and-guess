import socket
import struct
import json
import torch
from PIL import Image, ImageDraw
from transformers import CLIPProcessor, CLIPModel
import os

MODEL_PATH = "./model"
PORT = 5000

print(f"Loading CLIP model from {MODEL_PATH}...")
try:
    model = CLIPModel.from_pretrained(MODEL_PATH)
    processor = CLIPProcessor.from_pretrained(MODEL_PATH)
    print("Model loaded successfully.")
except Exception as e:
    print(f"Failed to load model: {e}")
    exit(1)

def reconstruct_image(drawing_data):
    # Create a white canvas
    img = Image.new('RGB', (800, 600), 'white')
    draw = ImageDraw.Draw(img)
    
    last_point = None
    # drawing_data is a list of dicts: {'x': int, 'y': int, 'action': int}
    # action: 1=Press, 2=Move, 3=Clear
    
    for point in drawing_data:
        x, y, action = point['x'], point['y'], point['action']
        
        if action == 1: # Press
            last_point = (x, y)
            draw.point((x, y), fill='black')
        elif action == 2: # Move
            if last_point:
                draw.line([last_point, (x, y)], fill='black', width=3)
            last_point = (x, y)
        elif action == 3: # Clear
            draw.rectangle([0, 0, 800, 600], fill='white')
            last_point = None
            
    return img

def handle_client(conn):
    try:
        # Read length of data
        len_bytes = conn.recv(4)
        if not len_bytes:
            return
        data_len = struct.unpack('!I', len_bytes)[0]
        
        # Read JSON data
        data_json = b''
        while len(data_json) < data_len:
            packet = conn.recv(4096)
            if not packet: break
            data_json += packet
            
        request = json.loads(data_json.decode('utf-8'))
        
        drawing_data = request['drawing']
        candidates = request['candidates'] # List of words
        target_word = request['target']
        
        # Reconstruct image
        image = reconstruct_image(drawing_data)
        
        # Prepare inputs
        inputs = processor(text=candidates, images=image, return_tensors="pt", padding=True)
        
        # Inference
        with torch.no_grad():
            outputs = model(**inputs)
            logits_per_image = outputs.logits_per_image # this is the image-text similarity score
            probs = logits_per_image.softmax(dim=1) # we can get probabilities
            
        # Get result
        probs_list = probs[0].tolist()
        best_idx = probs[0].argmax().item()
        predicted_word = candidates[best_idx]
        
        # Calculate similarity score for the target word
        # CLIP scores are unnormalized logits, but we can use probability or the raw score?
        # User asked for "percentage as similarity". Probability is good.
        # But if the set of candidates is small, probability might be skewed.
        # Let's use the probability of the target word.
        
        target_idx = -1
        if target_word in candidates:
            target_idx = candidates.index(target_word)
            
        target_score = 0
        if target_idx != -1:
            target_score = int(probs_list[target_idx] * 100)
            
        response = {
            'predicted_word': predicted_word,
            'is_correct': 1 if predicted_word == target_word else 0,
            'score': target_score
        }
        
        # Send response
        resp_json = json.dumps(response).encode('utf-8')
        conn.sendall(struct.pack('!I', len(resp_json)))
        conn.sendall(resp_json)
        
    except Exception as e:
        print(f"Error handling request: {e}")
    finally:
        conn.close()

def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(('127.0.0.1', PORT))
    server.listen(5)
    print(f"AI Service listening on port {PORT}")
    
    while True:
        conn, addr = server.accept()
        handle_client(conn)

if __name__ == '__main__':
    main()
