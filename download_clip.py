from transformers import CLIPProcessor, CLIPModel
import os

model_name = "openai/clip-vit-base-patch32"
cache_dir = "./server/model"

print(f"Downloading model {model_name} to {cache_dir}...")

if not os.path.exists(cache_dir):
    os.makedirs(cache_dir)

model = CLIPModel.from_pretrained(model_name, cache_dir=cache_dir)
processor = CLIPProcessor.from_pretrained(model_name, cache_dir=cache_dir)

model.save_pretrained(cache_dir)
processor.save_pretrained(cache_dir)

print("Model downloaded successfully!")

